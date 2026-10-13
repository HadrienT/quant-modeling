#ifndef QM_ENGINES_XVA_PILOT_TARGETS_HPP
#define QM_ENGINES_XVA_PILOT_TARGETS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/xva/future_value.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @file pilot_targets.hpp
     * @brief What a pilot path says about a trade at each date: the future
     *        cash flows a regression is fitted on, and their pathwise
     *        differential with respect to the state of that date
     *        (blueprint/wp/25-ml.md §3.1 and §3.4, lot M1).
     *
     * On one path, with D the deflator and c_j the cash flow at t_j:
     *
     *   Y_i = Σ_{j>i} c_j D_j / D_i,        V(t_i) = E[Y_i | state_i]
     *   Z_i = ∂Y_i / ∂state_i,              ∂V/∂state_i = E[Z_i | state_i]
     *
     * (differentiation under the expectation, Glasserman, Monte Carlo
     * Methods in Financial Engineering, §7.2; Huge & Savine, Differential
     * Machine Learning, 2020). The derivative is taken with the draws of
     * the path held fixed and the states before t_i untouched: moving
     * state_i moves every later state, and through them the later fixings
     * and deflators. What was fixed before t_i is a regressor of its own
     * (FutureValue::regressors), not a function of state_i.
     *
     * Matrices are stored like PilotPaths: element (path p, date i) is at
     * `p * dates + i`.
     */
    struct PilotTargets
    {
        std::size_t paths = 0;
        std::size_t dates = 0;
        /// Y_i.
        std::vector<Real> value;
        /// Z_i; empty when the differentials were not asked for.
        std::vector<Real> differential;
    };

    /**
     * @brief The targets of `trade` on the pilot paths, by one backward
     *        sweep per path: the differentials of every date cost one
     *        pass, whatever the number of dates.
     *
     * With F_i = Σ_{j>i} c_j D_j, H_i = dF_i/dstate_i, a_i = ∂state_i /
     * ∂state_{i-1} and s_i = ∂ ln D_i / ∂state_i:
     *
     *   H_i = g_i + a_{i+1} (H_{i+1} + ∂(c_{i+1} D_{i+1}) / ∂state_{i+1})
     *   Z_i = H_i / D_i - s_i Y_i
     *
     * where g_i gathers what the flows after t_i read directly from
     * state_i (a coupon fixed at t_i and paid later).
     *
     * @param trade         bound to the pilot's grid, and fitted if its
     *                      regime depends on a fitted exercise rule.
     * @param differentials false computes the values only.
     * @throws InvalidInput when differentials are asked for and the pilot
     *         has no sensitivities or the trade no cash-flow gradient.
     */
    PilotTargets pilot_targets(const FutureValue &trade, const PilotPaths &pilot,
                               bool differentials = true);

} // namespace quantModeling

#endif // QM_ENGINES_XVA_PILOT_TARGETS_HPP
