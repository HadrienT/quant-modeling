#ifndef GPU_SCRIPT_JIT_HPP
#define GPU_SCRIPT_JIT_HPP

#include <memory>
#include <string>
#include <vector>

/**
 * @file script_jit.hpp
 * @brief Scripts compiled for the GPU at run time (lot P1b of
 *        blueprint/wp/26-performance.md §5).
 *
 * A script's events, as CUDA source (scripting/cuda_codegen.hpp), are
 * compiled with NVRTC to LTO-IR and linked by nvJitLink with the embedded
 * LTO-IR of script_jit_kernel.cu into a cubin; the cubin is loaded on each
 * device through the driver API. Both libraries are linked statically, and
 * the driver's functions come from cudaGetDriverEntryPoint, so the module
 * needs no CUDA library beyond the driver the GPU backend already uses.
 *
 * Compiled code is cached in the process by its source (and the device's
 * architecture): the API reprices the same scripts many times, and the
 * compilation is paid once per script.
 *
 * And on disk (blueprint/wp/27-ir-ad.md §6.1, lot L1b): each cubin is
 * stored under the hash of its source, the architecture and the embedded
 * kernel it was linked with -- so a new build of the library whose kernel
 * changed never reads an old cubin -- and a later process loads it instead
 * of compiling. The file holds the source too, compared on load: a hash
 * collision cannot hand out the wrong code. Directory: QM_KERNEL_CACHE_DIR,
 * else $XDG_CACHE_HOME/quantmodeling/kernels, else
 * ~/.cache/quantmodeling/kernels; QM_KERNEL_CACHE=0 turns it off. A cache
 * that cannot be read or written is ignored: the script compiles.
 */

namespace quantModeling::gpu::jit
{

    /// False when QM_SCRIPT_JIT=0: every script then runs on the interpreter.
    bool enabled();

    /// A script's kernels, linked for one architecture.
    struct Compiled
    {
        std::vector<char> cubin;
        double compile_ms = 0.0; ///< NVRTC + nvJitLink, when it was built
        double nvrtc_ms = 0.0;   ///< of which NVRTC (the events)
        double link_ms = 0.0;    ///< of which nvJitLink (link-time optimisation, the kernel's code generation)
        bool from_disk = false;  ///< read from the kernel cache on disk; then compile_ms is the time to read it
    };

    /// The linked cubin of `events_source` for `device`'s architecture, or
    /// null and the reason in `why` (compilation or link failure). Kept for
    /// the life of the process; `cached` says whether this call found it built.
    std::shared_ptr<const Compiled> compile(const std::string &events_source, int device, std::string &why,
                                            bool *cached = nullptr);

    /// Forget the cubins held in memory (those on disk stay): the next
    /// compile() of a script reads the disk cache or builds. For tests.
    void forget_compiled();

    /// The directory of the kernel cache on disk; empty when it is off.
    std::string disk_cache_dir();

    /// What rebuild_stale() found in the cache directory.
    struct Rebuilt
    {
        int current = 0; ///< kernels already linked with this build's kernel
        int rebuilt = 0; ///< stale ones compiled again from their stored source
        int failed = 0;  ///< stale ones that no longer compile (removed)
    };

    /// Every script the cache holds a kernel of, linked with an older build
    /// of the library, compiled again for this one (for `device`'s
    /// architecture) and its stale file removed: after a deployment that
    /// changed the embedded kernel, the scripts seen before are ready before
    /// anyone asks for them. `stop`, when given, is polled between kernels.
    Rebuilt rebuild_stale(int device, const bool *stop = nullptr);

    /// The kernel `name` of `c`, loaded on `device` (made current), as a
    /// CUfunction; null and `why` on failure. Modules stay loaded for the
    /// life of the process.
    void *kernel(const std::shared_ptr<const Compiled> &c, int device, const char *name, std::string &why);

    /// cuLaunchKernel on the current device's default stream; throws
    /// GpuUnavailable on failure.
    void launch(void *function, unsigned grid, unsigned block, void **args);

} // namespace quantModeling::gpu::jit

#endif // GPU_SCRIPT_JIT_HPP
