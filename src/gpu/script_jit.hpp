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
    };

    /// The linked cubin of `events_source` for `device`'s architecture, or
    /// null and the reason in `why` (compilation or link failure). Kept for
    /// the life of the process; `cached` says whether this call found it built.
    std::shared_ptr<const Compiled> compile(const std::string &events_source, int device, std::string &why,
                                            bool *cached = nullptr);

    /// The kernel `name` of `c`, loaded on `device` (made current), as a
    /// CUfunction; null and `why` on failure. Modules stay loaded for the
    /// life of the process.
    void *kernel(const std::shared_ptr<const Compiled> &c, int device, const char *name, std::string &why);

    /// cuLaunchKernel on the current device's default stream; throws
    /// GpuUnavailable on failure.
    void launch(void *function, unsigned grid, unsigned block, void **args);

} // namespace quantModeling::gpu::jit

#endif // GPU_SCRIPT_JIT_HPP
