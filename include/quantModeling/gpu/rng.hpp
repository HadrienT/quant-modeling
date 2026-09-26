#ifndef GPU_RNG_HPP
#define GPU_RNG_HPP

#include <cstdint>
#include <vector>

#include "quantModeling/gpu/device.hpp"

/**
 * @file rng.hpp
 * @brief Draws computed on the device, copied back — the check that the
 *        GPU's integer arithmetic reproduces the CPU's generator bit for bit
 *        (blueprint/wp/19-gpu.md §10). Not a production path: Monte-Carlo
 *        kernels draw in registers and never ship draws to the host.
 */

namespace quantModeling::gpu
{

    /// philox_uniform(seed, first_path + p, j) for p < n_paths, j < draws,
    /// row-major by path. Throws GpuUnavailable without a device.
    std::vector<double> philox_uniforms(uint64_t seed, uint64_t first_path, uint32_t n_paths, uint32_t draws,
                                        int device = 0);

} // namespace quantModeling::gpu

#endif // GPU_RNG_HPP
