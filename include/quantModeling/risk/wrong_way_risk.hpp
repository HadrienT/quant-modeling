#ifndef QM_RISK_WRONG_WAY_RISK_HPP
#define QM_RISK_WRONG_WAY_RISK_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @file wrong_way_risk.hpp
     * @brief Wrong-way risk by the model of Hull & White (2012), "CVA and
     *        wrong-way risk" (blueprint/wp/23-xva.md §8, lot X6): the
     *        counterparty's hazard rate depends on the value of the netting
     *        set,
     *
     *   λ(t) = exp(a(t) + b V(t)).
     *
     * b > 0 is wrong-way risk: the counterparty is more likely to default in
     * the scenarios where it owes the bank more. b < 0 is right-way risk.
     * b = 0 is the independence every other figure of the library assumes.
     *
     * a(t) is not a free parameter: as in the paper, at each date it is the
     * one value for which the survival probability averaged over the
     * simulated scenarios is the market's,
     *
     *   mean over paths of exp(-∫_0^t λ) = S(t).
     *
     * So b moves default probability between scenarios without changing how
     * much of it there is, and b = 0 gives every path the market curve.
     * (The average is over the scenarios as they are simulated; the risky
     * discount factor E[D(t) exp(-∫λ)] then differs from P(0, t) S(t) by the
     * covariance of the survival with the discounting, a second-order term.)
     *
     * On the grid, λ is constant over (t_{i-1}, t_i] at exp(a_i + b V(t_i)),
     * and the survival of path p is q_i(p) = q_{i-1}(p) exp(-λ_i(p) Δt_i);
     * a_i is solved date after date. Like collateral, a post-processing of
     * the cube (ADR-X2).
     */

    /// The counterparty's survival on each path, under λ = exp(a + b V).
    struct PathwiseSurvival
    {
        std::vector<Time> times;
        std::size_t paths = 0;
        Real b = 0.0;
        /// a(t_i); empty when b = 0 (nothing to solve).
        std::vector<Real> a;
        /// survival[p * times.size() + i] = q_i(p): the probability, in the
        /// scenario of path p, that the counterparty has not defaulted by t_i.
        std::vector<Real> survival;

        std::size_t dates() const { return times.size(); }
        /// Survival at the date before i (1 before the first date).
        Real before(std::size_t p, std::size_t i) const
        {
            return i == 0 ? 1.0 : survival[p * dates() + i - 1];
        }
    };

    /**
     * @brief Solves a(t) on the paths of the cube and returns the survival
     *        of every path.
     *
     * @param paths  a risk-neutral simulation.
     * @param trades the netted trades whose value V drives the hazard; empty
     *               means all. The value is the trades' own, before
     *               collateral: what the counterparty owes is what is tied
     *               to its distress, whatever was posted against it.
     * @param b      per unit of currency: λ is multiplied by exp(b ΔV) when
     *               the value rises by ΔV.
     * @throws InvalidInput on a historical or empty cube, a non-finite b, or
     *         a date at which no a(t) gives the market survival: a survival
     *         curve that rises, or a b so large that all the default
     *         probability of the period would have to sit on a handful of
     *         paths.
     */
    PathwiseSurvival wrong_way_survival(const ExposurePaths &paths,
                                        const std::vector<std::size_t> &trades,
                                        const CreditCurve &counterparty, Real b);

} // namespace quantModeling

#endif // QM_RISK_WRONG_WAY_RISK_HPP
