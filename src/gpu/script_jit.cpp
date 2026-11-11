#include "script_jit.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
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

// The LTO-IR fatbin of script_jit_kernel.cu, embedded by CMake.
extern "C" const unsigned char qm_script_jit_fatbin[];
extern "C" const unsigned long long qm_script_jit_fatbin_size;

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
        std::shared_ptr<Compiled> build(const std::string &source, int arch, std::string &why)
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
                nvJitLinkAddData(h, NVJITLINK_INPUT_FATBIN, qm_script_jit_fatbin,
                                 static_cast<std::size_t>(qm_script_jit_fatbin_size),
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

        struct Cache
        {
            std::mutex mu;
            /// (architecture, source) -> its cubin, or the reason it failed.
            std::map<std::pair<int, std::string>, std::pair<std::shared_ptr<const Compiled>, std::string>> built;
            /// (cubin, device, kernel name) -> CUfunction.
            std::map<std::tuple<const Compiled *, int, std::string>, CUfunction> loaded;
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

    std::shared_ptr<const Compiled> compile(const std::string &events_source, int device, std::string &why,
                                            bool *cached)
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
        const auto key = std::make_pair(arch, events_source);
        const auto it = c.built.find(key);
        if (it != c.built.end())
        {
            why = it->second.second;
            if (cached)
                *cached = it->second.first != nullptr;
            return it->second.first;
        }
        std::shared_ptr<const Compiled> built = build(events_source, arch, why);
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
