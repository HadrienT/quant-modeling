#ifndef GPU_SCRIPT_HPP
#define GPU_SCRIPT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/scripting/bytecode.hpp"
#include "quantModeling/utils/accumulators.hpp"

/**
 * @file script.hpp
 * @brief A compiled script priced on the GPU (blueprint/wp/19-gpu.md §3-§5,
 *        lot G2).
 *
 * One thread per unit -- a path, or an antithetic pair -- simulates the
 * DeviceModel step by step and, at each event date, runs that event's
 * bytecode (scripting::run_event, the CPU's own interpreter) on the path's
 * spots, discount factors and numeraire. Units, Philox draws and reduction
 * tree are those of the CPU generic engine with mc_rng = Philox, which is
 * therefore the GPU's oracle.
 *
 * The per-path machine lives in fixed-size local arrays; a script or model
 * beyond these limits stays on the CPU (script_gpu_unsupported says why).
 */

namespace quantModeling::gpu
{

    struct ScriptLimits
    {
        static constexpr int kVars = 32;
        static constexpr int kStack = 16;
        static constexpr int kDegrees = 8;
        static constexpr int kIfSlots = 64;
        static constexpr int kIfModes = 8;
        static constexpr int kAssets = 8;
        static constexpr int kDiscounts = 8;
    };

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
        uint64_t n_units = 0;
        uint64_t seed = 1;
        bool antithetic = true;
        int device = 0;
        uint64_t max_blocks_per_launch = 0;
    };

    /// Payoff statistics over the units. Throws GpuUnavailable without a
    /// device, InvalidInput when script_gpu_unsupported() is not empty.
    WelfordAccumulator simulate_script(const ScriptGpuRequest &req);

} // namespace quantModeling::gpu

#endif // GPU_SCRIPT_HPP
