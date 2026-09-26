// The GPU entry points on a build without QM_ENABLE_CUDA: no device, and any
// GPU run is refused with GpuUnavailable (blueprint/wp/19-gpu.md §8).

#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/paths.hpp"
#include "quantModeling/gpu/rng.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"

namespace quantModeling::gpu
{

    bool compiled_with_cuda() noexcept
    {
        return false;
    }

    int device_count() noexcept
    {
        return 0;
    }

    std::string device_name(int)
    {
        return "";
    }

    std::size_t free_memory(int)
    {
        return 0;
    }

    mc::VanillaStats simulate_vanilla_terminal(const VanillaGpuRequest &)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

    mc::VanillaStats simulate_vanilla_sobol(const VanillaSobolGpuRequest &)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built "
                             "without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

    std::vector<double> terminal_spots(const PathModelSpec &, const std::vector<Time> &, uint32_t, uint64_t, int)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built "
                             "without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

    void warm_up(int)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

    std::vector<double> sobol_uniforms(int, uint64_t, uint32_t, uint32_t, int)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built "
                             "without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

    std::vector<double> philox_uniforms(uint64_t, uint64_t, uint32_t, uint32_t, int)
    {
        throw GpuUnavailable("GPU requested, but this server has no usable CUDA device (the pricing library was built without the CUDA backend: QM_ENABLE_CUDA=ON)");
    }

} // namespace quantModeling::gpu
