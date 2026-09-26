#ifndef GPU_PATHS_HPP
#define GPU_PATHS_HPP

#include <cstdint>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/models/equity/path_steps.hpp"

/**
 * @file paths.hpp
 * @brief Paths of the device models, one thread per path
 *        (blueprint/wp/19-gpu.md §4, lot G1).
 *
 * The models are a closed set chosen at launch, each step being the functor
 * the CPU model itself calls (models/equity/path_steps.hpp). Path p draws
 * its gaussians Φ⁻¹(Philox(seed, p, j)) with j = step × factors + factor:
 * the CPU model fed the same draws follows the same path, up to the last
 * ulps of exp and sqrt.
 *
 * G1 exposes terminal spots, the check that CPU and GPU share one law; the
 * script kernels of lot G2 build on the same kernel shape.
 */

namespace quantModeling::gpu
{

    enum class PathModel
    {
        LocalVol, ///< 1 factor; grid = local vol σ(K, T)
        Heston,   ///< 2 factors (spot, variance's independent part)
        SLV       ///< 2 factors; grid = leverage L(K, T), staircase in T
    };

    struct PathModelSpec
    {
        PathModel kind = PathModel::LocalVol;
        Real s0 = 100.0, r = 0.0, q = 0.0;
        mc::HestonParamsT<Real> heston{0.04, 1.0, 0.04, 0.5, -0.5};
        std::vector<Real> K, T_grid, grid; ///< grid K-major: grid[i * T_grid.size() + j]

        int factors() const { return kind == PathModel::LocalVol ? 1 : 2; }
    };

    /// S(times.back()) for paths 0 .. n_paths-1. `times`: the step grid
    /// (strictly increasing, > 0). Throws GpuUnavailable without a device.
    std::vector<double> terminal_spots(const PathModelSpec &model, const std::vector<Time> &times, uint32_t n_paths,
                                       uint64_t seed, int device = 0);

} // namespace quantModeling::gpu

#endif // GPU_PATHS_HPP
