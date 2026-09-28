#ifndef GPU_DEVICE_HPP
#define GPU_DEVICE_HPP

#include <cstddef>
#include <string>
#include <vector>

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

    /// The devices a run shares its logical blocks between (lot G4): the
    /// first `max_gpus` usable ones, all of them for 0. The result does not
    /// depend on how many (blueprint §7), so more is only faster.
    inline std::vector<int> devices_for(int max_gpus)
    {
        const int n = device_count();
        const int k = max_gpus > 0 && max_gpus < n ? max_gpus : n;
        std::vector<int> out;
        for (int d = 0; d < k; ++d)
            out.push_back(d);
        return out.empty() ? std::vector<int>{0} : out;
    }

    /// A card only joins a run with at least this many logical blocks to do:
    /// fewer leave a V100's 80 SMs idle, and a second card's start-up then
    /// costs more than it saves (measured: a 2-block Sobol replicate, 6 ms
    /// on one card, 13 ms on two). The result is the same bits either way.
    constexpr unsigned long long kMinBlocksPerDevice = 128;

    /// How many of `available` devices a run of n_blocks logical blocks uses.
    inline std::size_t devices_used(std::size_t available, unsigned long long n_blocks)
    {
        const unsigned long long useful = n_blocks / kMinBlocksPerDevice > 0 ? n_blocks / kMinBlocksPerDevice : 1;
        const unsigned long long n = available < useful ? available : useful;
        return static_cast<std::size_t>(n > 0 ? n : 1);
    }

    /// "Tesla V100-PCIE-16GB" or "2 x Tesla V100-PCIE-16GB".
    inline std::string devices_label(const std::vector<int> &devices)
    {
        const std::string name = device_name(devices.empty() ? 0 : devices.front());
        return devices.size() > 1 ? std::to_string(devices.size()) + " x " + name : name;
    }

} // namespace quantModeling::gpu

#endif // GPU_DEVICE_HPP
