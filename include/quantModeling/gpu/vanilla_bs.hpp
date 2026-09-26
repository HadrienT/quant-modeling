#ifndef GPU_VANILLA_BS_HPP
#define GPU_VANILLA_BS_HPP

#include <cstdint>

#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/gpu/device.hpp"

/**
 * @file vanilla_bs.hpp
 * @brief European vanilla under flat Black-Scholes on the GPU
 *        (blueprint/wp/19-gpu.md lot G0).
 *
 * Runs mc::VanillaPhiloxUnit — the same code the CPU runs through
 * mc::reduce_logical_blocks — one CUDA block per logical block. Only the
 * per-block Welford partials come back to the host, which folds them in
 * block order.
 */

namespace quantModeling::gpu
{

    struct VanillaGpuRequest
    {
        mc::VanillaTerminalSpec spec;
        OptionType type = OptionType::Call;
        bool antithetic = true;
        Real is_shift = 0.0; ///< 0 = no importance sampling
        uint64_t n_units = 0;
        uint64_t seed = 0;
        int device = 0;
        /// Upper bound on logical blocks per kernel launch; 0 = computed from
        /// free device memory. Tests set it to force several launches.
        uint64_t max_blocks_per_launch = 0;
    };

    /// Throws GpuUnavailable when no device can run it.
    mc::VanillaStats simulate_vanilla_terminal(const VanillaGpuRequest &req);

    /// One randomised-QMC replicate: points 0 .. n_points-1 of a
    /// one-dimensional Sobol sequence given by its 32 direction integers and
    /// digital shift (SobolSequence::directions() / shifts()).
    struct VanillaSobolGpuRequest
    {
        mc::VanillaTerminalSpec spec;
        OptionType type = OptionType::Call;
        Real is_shift = 0.0;
        uint32_t directions[32] = {};
        uint32_t shift = 0;
        uint64_t n_points = 0;
        int device = 0;
        uint64_t max_blocks_per_launch = 0;
    };

    mc::VanillaStats simulate_vanilla_sobol(const VanillaSobolGpuRequest &req);

    /// Create the CUDA context on `device` and load every vanilla kernel, so
    /// that the first priced request does not pay for them (a few hundred
    /// milliseconds, once per process). A timing shown to a user must be the
    /// pricing's, not the driver's start-up.
    void warm_up(int device = 0);

} // namespace quantModeling::gpu

#endif // GPU_VANILLA_BS_HPP
