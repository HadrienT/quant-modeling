#ifndef QM_ENGINES_MC_SPOT_CONTROLS_HPP
#define QM_ENGINES_MC_SPOT_CONTROLS_HPP

#include <algorithm>
#include <cstddef>
#include <string>

#include "quantModeling/core/types.hpp"
#include <vector>
#include "quantModeling/utils/variance_reduction/multi_control.hpp"

/**
 * @file spot_controls.hpp
 * @brief The generic control variates of a simulation: deflated spots at
 *        event dates (blueprint/wp/19-gpu.md §2.5, lot G3).
 *
 * Whatever the payoff, S_a(t) / N(t) -- asset a's spot at an event date,
 * deflated by the numeraire -- is a martingale under the pricing measure,
 * with a mean the model knows exactly (ISimulationModel::deflated_spot_mean:
 * S0 e^{-q t} for the diffusions here, whose log-Euler steps keep the
 * martingale property on the grid). A payoff is correlated with the spots it
 * reads, so these are controls for every script at once (Glasserman §4.1).
 *
 * At most kMaxControls of them: with several assets, the last event date of
 * each; otherwise event dates spread evenly over the timeline, the last one
 * always included -- the regression then picks the combination that best
 * explains the payoff (an average for an Asian, the last spot for a
 * European, the observation dates for an autocall).
 */

namespace quantModeling::mc
{

    struct SpotControls
    {
        static constexpr int kMax = MultiControlAccumulator::kMaxControls;
        int n = 0;
        int event[kMax] = {};
        int asset[kMax] = {};
    };

    struct SpotControlSet
    {
        SpotControls sel;
        Real mean[SpotControls::kMax] = {};
        std::string why_none; ///< empty when controls were chosen
    };

    /// Choose the controls for a product whose events fall at `times`, on
    /// `model` (an ISimulationModel, after init()).
    template <class Model>
    SpotControlSet choose_spot_controls(const std::vector<Time> &times, const Model &model)
    {
        SpotControlSet out;
        const int n_events = static_cast<int>(times.size());
        const int n_assets = static_cast<int>(model.n_underlyings());
        if (n_events == 0 || n_assets == 0)
        {
            out.why_none = "no event date to observe a spot on";
            return out;
        }
        constexpr int kMax = SpotControls::kMax;
        const int per_asset = std::max(1, kMax / n_assets);
        const int dates = std::min(per_asset, n_events);
        for (int a = 0; a < n_assets && out.sel.n < kMax; ++a)
            for (int k = 0; k < dates && out.sel.n < kMax; ++k)
            {
                // evenly spread, ending on the last event
                const int e = static_cast<int>((static_cast<long>(k + 1) * n_events) / dates) - 1;
                Real mean = 0.0;
                if (!model.deflated_spot_mean(static_cast<std::size_t>(a), times[static_cast<std::size_t>(e)], mean))
                {
                    out.sel.n = 0;
                    out.why_none = "the model has no known martingale to use as a control";
                    return out;
                }
                out.sel.event[out.sel.n] = e;
                out.sel.asset[out.sel.n] = a;
                out.mean[out.sel.n] = mean;
                ++out.sel.n;
            }
        return out;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SPOT_CONTROLS_HPP
