#include "script_jit.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <mutex>
#include <utility>

#include <cuda.h>
#include <cudaTypedefs.h>
#include <cuda_runtime_api.h>
#include <nvJitLink.h>
#include <nvrtc.h>

#include "quantModeling/gpu/device.hpp"

// The LTO-IR fatbins of script_jit_kernel.cu, one per kernel (0 prices,
// 1 tangent, 2 adjoint), embedded by CMake.
extern "C" const unsigned char qm_script_jit_fatbin_0[];
extern "C" const unsigned long long qm_script_jit_fatbin_0_size;
extern "C" const unsigned char qm_script_jit_fatbin_1[];
extern "C" const unsigned long long qm_script_jit_fatbin_1_size;
extern "C" const unsigned char qm_script_jit_fatbin_2[];
extern "C" const unsigned long long qm_script_jit_fatbin_2_size;

namespace quantModeling::gpu::jit
{

    namespace
    {
        /// The driver functions this module needs, from the runtime's entry
        /// points: no link against libcuda.so, whose absence must leave the
        /// module importable (the CPU-only fallback).
        struct Driver
        {
            PFN_cuModuleLoadData load = nullptr;
            PFN_cuModuleGetFunction get_function = nullptr;
            PFN_cuLaunchKernel launch = nullptr;
            PFN_cuGetErrorString error_string = nullptr;
            bool ok = false;

            Driver()
            {
                auto get = [](const char *symbol, void **fn)
                {
                    cudaDriverEntryPointQueryResult status{};
                    return cudaGetDriverEntryPoint(symbol, fn, cudaEnableDefault, &status) == cudaSuccess &&
                           status == cudaDriverEntryPointSuccess && *fn != nullptr;
                };
                ok = get("cuModuleLoadData", reinterpret_cast<void **>(&load)) &&
                     get("cuModuleGetFunction", reinterpret_cast<void **>(&get_function)) &&
                     get("cuLaunchKernel", reinterpret_cast<void **>(&launch)) &&
                     get("cuGetErrorString", reinterpret_cast<void **>(&error_string));
            }

            std::string message(CUresult r) const
            {
                const char *s = nullptr;
                if (error_string && error_string(r, &s) == CUDA_SUCCESS && s)
                    return s;
                return "CUDA driver error " + std::to_string(static_cast<int>(r));
            }
        };

        const Driver &driver()
        {
            static const Driver d;
            return d;
        }

        struct Fatbin
        {
            const unsigned char *data;
            std::size_t size;
        };

        Fatbin fatbin_of(int kernel)
        {
            switch (kernel)
            {
                case 1:
                    return {qm_script_jit_fatbin_1, static_cast<std::size_t>(qm_script_jit_fatbin_1_size)};
                case 2:
                    return {qm_script_jit_fatbin_2, static_cast<std::size_t>(qm_script_jit_fatbin_2_size)};
                default:
                    return {qm_script_jit_fatbin_0, static_cast<std::size_t>(qm_script_jit_fatbin_0_size)};
            }
        }

        /// The compute capability of `device` as a number (70 for a V100).
        int arch_of(int device)
        {
            int major = 0, minor = 0;
            if (cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor, device) != cudaSuccess ||
                cudaDeviceGetAttribute(&minor, cudaDevAttrComputeCapabilityMinor, device) != cudaSuccess)
                return 0;
            return 10 * major + minor;
        }

        std::string nvrtc_log(nvrtcProgram p)
        {
            std::size_t n = 0;
            if (nvrtcGetProgramLogSize(p, &n) != NVRTC_SUCCESS || n <= 1)
                return "";
            std::string log(n, '\0');
            nvrtcGetProgramLog(p, log.data());
            log.resize(n - 1);
            return log;
        }

        std::string link_log(nvJitLinkHandle h)
        {
            std::size_t n = 0;
            if (nvJitLinkGetErrorLogSize(h, &n) != NVJITLINK_SUCCESS || n <= 1)
                return "";
            std::string log(n, '\0');
            nvJitLinkGetErrorLog(h, log.data());
            log.resize(n - 1);
            return log;
        }

        /// The events' LTO-IR, then the link with the kernel's.
        std::shared_ptr<Compiled> build(const std::string &source, int kernel, int arch, std::string &why)
        {
            const auto t0 = std::chrono::steady_clock::now();
            nvrtcProgram prog = nullptr;
            if (nvrtcCreateProgram(&prog, source.c_str(), "qm_script_events.cu", 0, nullptr, nullptr) !=
                NVRTC_SUCCESS)
            {
                why = "NVRTC could not create the program";
                return nullptr;
            }
            const std::string arch_opt = "-arch=compute_" + std::to_string(arch);
            // No FMA contraction, as nvcc compiles the interpreter
            // (CMakeLists.txt): the bits must not depend on the optimiser.
            const char *opts[] = {arch_opt.c_str(), "-dlto", "-rdc=true", "-std=c++17", "--fmad=false"};
            const nvrtcResult rc = nvrtcCompileProgram(prog, 5, opts);
            std::vector<char> ir;
            if (rc == NVRTC_SUCCESS)
            {
                std::size_t n = 0;
                if (nvrtcGetLTOIRSize(prog, &n) == NVRTC_SUCCESS)
                {
                    ir.resize(n);
                    if (nvrtcGetLTOIR(prog, ir.data()) != NVRTC_SUCCESS)
                        ir.clear();
                }
            }
            else
                why = "NVRTC failed to compile the script: " + nvrtc_log(prog);
            nvrtcDestroyProgram(&prog);
            if (ir.empty())
            {
                if (why.empty())
                    why = "NVRTC gave no LTO-IR";
                return nullptr;
            }

            const auto t1 = std::chrono::steady_clock::now();
            const std::string sm_opt = "-arch=sm_" + std::to_string(arch);
            const char *link_opts[] = {sm_opt.c_str(), "-lto", "-fma=0"};
            nvJitLinkHandle h = nullptr;
            if (nvJitLinkCreate(&h, 3, link_opts) != NVJITLINK_SUCCESS)
            {
                why = "nvJitLink could not start";
                return nullptr;
            }
            auto out = std::make_shared<Compiled>();
            const bool linked =
                nvJitLinkAddData(h, NVJITLINK_INPUT_FATBIN, fatbin_of(kernel).data, fatbin_of(kernel).size,
                                 "script_jit_kernel") == NVJITLINK_SUCCESS &&
                nvJitLinkAddData(h, NVJITLINK_INPUT_LTOIR, ir.data(), ir.size(), "qm_script_events") ==
                    NVJITLINK_SUCCESS &&
                nvJitLinkComplete(h) == NVJITLINK_SUCCESS;
            if (linked)
            {
                std::size_t n = 0;
                if (nvJitLinkGetLinkedCubinSize(h, &n) == NVJITLINK_SUCCESS)
                {
                    out->cubin.resize(n);
                    if (nvJitLinkGetLinkedCubin(h, out->cubin.data()) != NVJITLINK_SUCCESS)
                        out->cubin.clear();
                }
            }
            else
                why = "nvJitLink failed to link the script: " + link_log(h);
            nvJitLinkDestroy(&h);
            if (out->cubin.empty())
            {
                if (why.empty())
                    why = "nvJitLink gave no cubin";
                return nullptr;
            }
            const auto t2 = std::chrono::steady_clock::now();
            out->nvrtc_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            out->link_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            out->compile_ms = out->nvrtc_ms + out->link_ms;
            return out;
        }

        // ── the cache on disk (WP 27 lot L1b) ───────────────────────────────

        constexpr char kMagic[8] = {'Q', 'M', 'K', 'E', 'R', 'N', '0', '2'};

        std::uint64_t fnv1a(const void *data, std::size_t n, std::uint64_t h = 0xcbf29ce484222325ull)
        {
            const auto *p = static_cast<const unsigned char *>(data);
            for (std::size_t i = 0; i < n; ++i)
            {
                h ^= p[i];
                h *= 0x100000001b3ull;
            }
            return h;
        }

        /// The embedded kernels' hash: a cubin is only valid for the kernel
        /// it was linked with.
        std::uint64_t kernel_hash(int kernel)
        {
            static const std::uint64_t h[3] = {fnv1a(fatbin_of(0).data, fatbin_of(0).size),
                                               fnv1a(fatbin_of(1).data, fatbin_of(1).size),
                                               fnv1a(fatbin_of(2).data, fatbin_of(2).size)};
            return h[kernel >= 0 && kernel < 3 ? kernel : 0];
        }

        std::filesystem::path disk_path(const std::string &source, int kernel, int arch)
        {
            const std::string dir = disk_cache_dir();
            if (dir.empty())
                return {};
            std::uint64_t h = fnv1a(&arch, sizeof arch, kernel_hash(kernel));
            h = fnv1a(source.data(), source.size(), h);
            char name[40];
            std::snprintf(name, sizeof name, "%016llx-sm%d.qmk", static_cast<unsigned long long>(h), arch);
            return std::filesystem::path(dir) / name;
        }

        /// The cubin stored for exactly this source, architecture and
        /// kernel; null when there is none (or the file is not ours).
        std::shared_ptr<Compiled> read_disk(const std::string &source, int kernel, int arch)
        {
            const std::filesystem::path path = disk_path(source, kernel, arch);
            if (path.empty())
                return nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return nullptr;
            char magic[sizeof kMagic];
            std::uint64_t linked_with = 0, source_size = 0, cubin_size = 0;
            std::int32_t stored_arch = 0, stored_kernel = -1;
            in.read(magic, sizeof magic);
            in.read(reinterpret_cast<char *>(&linked_with), sizeof linked_with);
            in.read(reinterpret_cast<char *>(&stored_arch), sizeof stored_arch);
            in.read(reinterpret_cast<char *>(&stored_kernel), sizeof stored_kernel);
            in.read(reinterpret_cast<char *>(&source_size), sizeof source_size);
            in.read(reinterpret_cast<char *>(&cubin_size), sizeof cubin_size);
            if (!in || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || linked_with != kernel_hash(kernel) ||
                stored_arch != arch || stored_kernel != kernel || source_size != source.size() || cubin_size == 0 ||
                cubin_size > (std::uint64_t{1} << 30))
                return nullptr;
            std::string stored(source_size, '\0');
            in.read(stored.data(), static_cast<std::streamsize>(source_size));
            if (!in || stored != source)
                return nullptr;
            auto out = std::make_shared<Compiled>();
            out->cubin.resize(cubin_size);
            in.read(out->cubin.data(), static_cast<std::streamsize>(cubin_size));
            if (!in)
                return nullptr;
            out->from_disk = true;
            out->compile_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return out;
        }

        /// Written to a temporary file and renamed: a reader never sees half
        /// a file, and two processes writing the same kernel write the same
        /// bytes. Failures are silent (the cache is an optimisation).
        void write_disk(const std::string &source, int kernel, int arch, const Compiled &c)
        {
            const std::filesystem::path path = disk_path(source, kernel, arch);
            if (path.empty())
                return;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
                return;
            std::filesystem::path tmp = path;
            tmp += ".tmp" + std::to_string(static_cast<long long>(
                                std::chrono::steady_clock::now().time_since_epoch().count()));
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return;
                const std::uint64_t linked_with = kernel_hash(kernel), source_size = source.size(),
                                    cubin_size = c.cubin.size();
                const std::int32_t stored_arch = arch, stored_kernel = kernel;
                out.write(kMagic, sizeof kMagic);
                out.write(reinterpret_cast<const char *>(&linked_with), sizeof linked_with);
                out.write(reinterpret_cast<const char *>(&stored_arch), sizeof stored_arch);
                out.write(reinterpret_cast<const char *>(&stored_kernel), sizeof stored_kernel);
                out.write(reinterpret_cast<const char *>(&source_size), sizeof source_size);
                out.write(reinterpret_cast<const char *>(&cubin_size), sizeof cubin_size);
                out.write(source.data(), static_cast<std::streamsize>(source.size()));
                out.write(c.cubin.data(), static_cast<std::streamsize>(c.cubin.size()));
                if (!out)
                {
                    out.close();
                    std::filesystem::remove(tmp, ec);
                    return;
                }
            }
            std::filesystem::rename(tmp, path, ec);
            if (ec)
                std::filesystem::remove(tmp, ec);
        }

        struct Cache
        {
            std::mutex mu;
            /// (architecture, source) -> its cubin, or the reason it failed.
            std::map<std::tuple<int, int, std::string>, std::pair<std::shared_ptr<const Compiled>, std::string>> built;
            /// (cubin, device, kernel name) -> CUfunction.
            std::map<std::tuple<const Compiled *, int, std::string>, CUfunction> loaded;
            /// Cubins forgotten by forget_compiled(), kept alive for `loaded`.
            std::vector<std::shared_ptr<const Compiled>> retired;
        };

        Cache &cache()
        {
            static Cache c;
            return c;
        }
    } // namespace

    bool enabled()
    {
        const char *v = std::getenv("QM_SCRIPT_JIT");
        return !(v && std::strcmp(v, "0") == 0);
    }

    std::string disk_cache_dir()
    {
        const char *on = std::getenv("QM_KERNEL_CACHE");
        if (on && std::strcmp(on, "0") == 0)
            return "";
        if (const char *dir = std::getenv("QM_KERNEL_CACHE_DIR"); dir && *dir)
            return dir;
        if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
            return std::string(xdg) + "/quantmodeling/kernels";
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::string(home) + "/.cache/quantmodeling/kernels";
        return "";
    }

    Rebuilt rebuild_stale(int device, const bool *stop)
    {
        Rebuilt out;
        const std::string dir = disk_cache_dir();
        const int arch = driver().ok ? arch_of(device) : 0;
        std::error_code ec;
        if (dir.empty() || arch == 0 || !std::filesystem::is_directory(dir, ec))
            return out;
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            if (entry.path().extension() == ".qmk")
                files.push_back(entry.path());
        for (const std::filesystem::path &path : files)
        {
            if (stop && *stop)
                break;
            std::string source;
            std::int32_t kernel = -1;
            {
                std::ifstream in(path, std::ios::binary);
                char magic[sizeof kMagic];
                std::uint64_t linked_with = 0, source_size = 0, cubin_size = 0;
                std::int32_t stored_arch = 0;
                in.read(magic, sizeof magic);
                in.read(reinterpret_cast<char *>(&linked_with), sizeof linked_with);
                in.read(reinterpret_cast<char *>(&stored_arch), sizeof stored_arch);
                in.read(reinterpret_cast<char *>(&kernel), sizeof kernel);
                in.read(reinterpret_cast<char *>(&source_size), sizeof source_size);
                in.read(reinterpret_cast<char *>(&cubin_size), sizeof cubin_size);
                if (!in || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || stored_arch != arch ||
                    source_size > (std::uint64_t{1} << 26))
                    continue; // not ours, or another card's
                if (kernel < 0 || kernel > 2)
                    continue;
                if (linked_with == kernel_hash(kernel))
                {
                    ++out.current;
                    continue;
                }
                source.resize(source_size);
                in.read(source.data(), static_cast<std::streamsize>(source_size));
                if (!in)
                    continue;
            }
            std::string why;
            const bool ok = compile(source, kernel, device, why) != nullptr; // reads, or builds and writes, the current file
            ok ? ++out.rebuilt : ++out.failed;
            std::filesystem::remove(path, ec);
        }
        return out;
    }

    void forget_compiled()
    {
        Cache &c = cache();
        const std::lock_guard<std::mutex> lock(c.mu);
        // The loaded modules keep their cubins alive (`loaded` is keyed by
        // the owner's address): only the lookup by source is dropped.
        for (auto &entry : c.built)
            if (entry.second.first)
                c.retired.push_back(entry.second.first);
        c.built.clear();
    }

    std::shared_ptr<const Compiled> compile(const std::string &events_source, int kernel, int device,
                                            std::string &why, bool *cached)
    {
        if (cached)
            *cached = false;
        if (!driver().ok)
        {
            why = "the CUDA driver does not expose the module functions";
            return nullptr;
        }
        const int arch = arch_of(device);
        if (arch == 0)
        {
            why = "the device's architecture is unknown";
            return nullptr;
        }
        Cache &c = cache();
        // One compilation at a time: the same script asked twice at once is
        // built once.
        const std::lock_guard<std::mutex> lock(c.mu);
        const auto key = std::make_tuple(kernel, arch, events_source);
        const auto it = c.built.find(key);
        if (it != c.built.end())
        {
            why = it->second.second;
            if (cached)
                *cached = it->second.first != nullptr;
            return it->second.first;
        }
        std::shared_ptr<const Compiled> built = read_disk(events_source, kernel, arch);
        if (!built)
        {
            built = build(events_source, kernel, arch, why);
            if (built)
                write_disk(events_source, kernel, arch, *built);
        }
        c.built.emplace(key, std::make_pair(built, why));
        return built;
    }

    void *kernel(const std::shared_ptr<const Compiled> &c, int device, const char *name, std::string &why)
    {
        const Driver &d = driver();
        Cache &cc = cache();
        const std::lock_guard<std::mutex> lock(cc.mu);
        const Compiled *owner = c.get(); // cached for the life of the process
        const auto key = std::make_tuple(owner, device, std::string(name));
        const auto it = cc.loaded.find(key);
        if (it != cc.loaded.end())
            return reinterpret_cast<void *>(it->second);

        if (cudaSetDevice(device) != cudaSuccess || cudaFree(nullptr) != cudaSuccess) // primary context current
        {
            why = "cannot make device " + std::to_string(device) + " current";
            return nullptr;
        }
        CUmodule module = nullptr;
        CUresult r = d.load(&module, owner->cubin.data());
        if (r != CUDA_SUCCESS)
        {
            why = "loading the compiled script failed: " + d.message(r);
            return nullptr;
        }
        CUfunction fn = nullptr;
        r = d.get_function(&fn, module, name);
        if (r != CUDA_SUCCESS)
        {
            why = std::string("the compiled script has no kernel ") + name + ": " + d.message(r);
            return nullptr;
        }
        cc.loaded.emplace(key, fn);
        return reinterpret_cast<void *>(fn);
    }

    void launch(void *function, unsigned grid, unsigned block, void **args)
    {
        const Driver &d = driver();
        const CUresult r =
            d.launch(reinterpret_cast<CUfunction>(function), grid, 1, 1, block, 1, 1, 0, nullptr, args, nullptr);
        if (r != CUDA_SUCCESS)
            throw GpuUnavailable("CUDA error in compiled script launch: " + d.message(r));
    }

} // namespace quantModeling::gpu::jit
