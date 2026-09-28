#ifndef GPU_SCRIPT_HPP
#define GPU_SCRIPT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/script_path.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/scripting/bytecode.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/variance_reduction/multi_control.hpp"

/**
 * @file script.hpp
 * @brief A compiled script priced on the GPU, and its model risks
 *        (blueprint/wp/19-gpu.md §3-§6, lots G2-G3).
 *
 * One thread per unit -- a path, or an antithetic pair -- runs
 * mc::script_path (engines/mc/script_path.hpp): the DeviceModel stepped by
 * the shared functors, each event's bytecode run by the CPU's own
 * interpreter. Units, Philox draws and reduction tree are those of the CPU
 * generic engine with mc_rng = Philox, which is therefore the GPU's oracle.
 *
 * Lot G3 adds, on the same kernel shape:
 *  - the generic variance reduction: spot control variates (the regression
 *    statistics of each unit) and a stratified terminal value (independent
 *    replicates of `strata` units, one launch each);
 *  - model risks without a tape: forward-mode duals for Black-Scholes and
 *    Heston, the per-path adjoint for local vol (every sigma_loc point).
 *
 * The per-path machine lives in fixed-size local arrays; a script or model
 * beyond these limits stays on the CPU (script_gpu_unsupported says why).
 */

namespace quantModeling::gpu
{

    using ScriptLimits = mc::ScriptLimits;

    /// "" when the GPU engine can run this program on this model, else the
    /// reason, in words a user can read.
    inline std::string script_gpu_unsupported(const scripting::Program &p, const DeviceModel &m,
                                              const std::vector<std::vector<Time>> &discount_mats)
    {
        using L = ScriptLimits;
        if (p.code.empty())
            return "the script is not compiled to bytecode";
        if (p.n_vars > L::kVars)
            return "the script has more than " + std::to_string(L::kVars) + " variables";
        if (p.max_stack > L::kStack || p.max_degrees > L::kDegrees)
            return "the script's expressions are nested too deeply for the GPU kernel";
        if (p.n_if_slots > L::kIfSlots || p.n_if_modes > L::kIfModes)
            return "the script's fuzzy ifs are nested too deeply for the GPU kernel";
        if (m.n_assets > L::kAssets)
            return "more than " + std::to_string(L::kAssets) + " underlyings";
        for (const auto &mats : discount_mats)
            if (static_cast<int>(mats.size()) > L::kDiscounts)
                return "more than " + std::to_string(L::kDiscounts) + " df() lookups on one date";
        return "";
    }

    struct ScriptGpuRequest
    {
        const scripting::Program *program = nullptr;
        const DeviceModel *model = nullptr;
        std::vector<Real> baseline;                   ///< starting variables; empty = zeros
        std::vector<std::vector<Time>> discount_mats; ///< per event: the df() maturities
        uint64_t n_units = 0;                         ///< per replicate when stratified
        uint64_t seed = 1;
        bool antithetic = true;
        int device = 0;
        uint64_t max_blocks_per_launch = 0;

        /// Spot control variates (n > 0): each unit reports (payoff,
        /// controls) for the regression estimate.
        mc::SpotControls controls;
        /// Stratified terminal value: `replicates` launches of n_units
        /// units, unit i of each in stratum i of n_units.
        bool stratified = false;
        int replicates = 1;
    };

    /// Per replicate (one entry when not stratified): the payoff statistics,
    /// in `plain` without controls, in `controlled` with them (or
    /// `stratified_controlled`, for the within-stratum slope).
    struct ScriptGpuStats
    {
        std::vector<WelfordAccumulator> plain;
        std::vector<MultiControlAccumulator> controlled;
        std::vector<StratifiedControlAccumulator> stratified_controlled; ///< stratified and controlled
    };

    /// Throws GpuUnavailable without a device, InvalidInput when
    /// script_gpu_unsupported() is not empty.
    ScriptGpuStats simulate_script(const ScriptGpuRequest &req);

    /// Forward-mode risks (Black-Scholes, one or two assets, and Heston):
    /// the price's Welford, then one per derivative, in the CPU model's label
    /// order (mc::dual_directions). One path per unit, no mirror.
    std::vector<WelfordAccumulator> simulate_script_duals(const ScriptGpuRequest &req);

    struct ScriptAdjointGpuResult
    {
        WelfordAccumulator price;
        std::vector<Real> risks;           ///< spot, rate, div, then sigma_loc K-major
        std::vector<Real> risk_std_errors; ///< over batches of 512 paths (one warp)
        long long batches = 0;
    };

    /// The per-path adjoint of a script under local vol: d price / d (spot,
    /// rate, div, every sigma_loc point). One path per unit, no mirror.
    ScriptAdjointGpuResult simulate_script_adjoint(const ScriptGpuRequest &req);

} // namespace quantModeling::gpu

#endif // GPU_SCRIPT_HPP
