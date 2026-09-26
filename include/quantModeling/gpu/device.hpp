#ifndef GPU_DEVICE_HPP
#define GPU_DEVICE_HPP

#include <cstddef>
#include <string>

#include "quantModeling/core/types.hpp"

/**
 * @file device.hpp
 * @brief Run-time GPU detection (blueprint/wp/19-gpu.md §8).
 *
 * No CUDA header leaks from here: the library compiles the same with or
 * without QM_ENABLE_CUDA. Without it, gpu_device_count() is 0 and every GPU
 * entry point throws GpuUnavailable.
 */

namespace quantModeling::gpu
{

    /// Thrown when a GPU run is requested and cannot happen (no CUDA build,
    /// no device, or a CUDA error).
    struct GpuUnavailable : PricingError
    {
        using PricingError::PricingError;
    };

    /// True when the library was compiled with the CUDA backend.
    bool compiled_with_cuda() noexcept;

    /// Usable CUDA devices, probed once (0 on a CPU-only build, with no
    /// driver, or with CUDA_VISIBLE_DEVICES empty).
    int device_count() noexcept;

    /// Device name, e.g. "Tesla V100-PCIE-16GB" ("" if out of range).
    std::string device_name(int device);

    /// Free device memory in bytes right now (cudaMemGetInfo). The cards are
    /// shared with the scripting assistant's LLM: batch sizes are computed
    /// from this, never assumed.
    std::size_t free_memory(int device);

} // namespace quantModeling::gpu

#endif // GPU_DEVICE_HPP
