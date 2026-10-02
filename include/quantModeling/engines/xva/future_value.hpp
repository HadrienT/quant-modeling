#ifndef QM_ENGINES_XVA_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_FUTURE_VALUE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/utils/bucketed_regression.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Independent paths of the model state on which the trades valued
     *        by regression are fitted before the main simulation (American
     *        Monte-Carlo, blueprint/wp/23-xva.md §13.4, lot X4).
     *
     * They are drawn under the pricing measure whatever the measure of the
     * main simulation: a regression estimates a price, E[future flows |
     * state], and a price is an expectation under the pricing measure. Only
     * the *ratio* of two deflators of a path is meaningful: the value at
     * t_i of a flow X paid at t_j is E[X deflator(j) / deflator(i) | t_i].
     *
     * Matrices are stored row by row: element (path p, date i) is at
     * `p * dates() + i`.
     */
    struct PilotPaths
    {
        std::size_t paths = 0;
        /// The exposure grid, t_1 < ... < t_n.
        std::vector<Time> times;
        /// The model state at each date.
        std::vector<Real> state;
        /// 1 / numeraire, up to a constant: P(0, T*) / P(t_i, T*) under the
        /// T*-forward measure.
        std::vector<Real> deflator;

        std::size_t dates() const { return times.size(); }
        const Real *state_row(std::size_t p) const { return state.data() + p * dates(); }
        const Real *deflator_row(std::size_t p) const { return deflator.data() + p * dates(); }
    };

    /**
     * @brief The "future value" of one trade: what it is worth at each date
     *        of the exposure grid on one simulated path
     *        (blueprint/wp/23-xva.md §13.2, lot X1).
     *
     * This is the seam that keeps the exposure engine free of any product:
     * the engine simulates the model state and asks each trade for its value;
     * it never learns what the trade is. One implementation exists per
     * (instrument, model) pair — closed form when there is one (ADR-X1),
     * regression otherwise (lot X4).
     *
     * `state` is the path of the model state on the grid: state[j] is the
     * state at grid[j], valid for j <= i when the method is called for date
     * i. Earlier dates are needed because a trade can be path-dependent in
     * the plainest way: a floating coupon is fixed before it is paid, a
     * swaption has been exercised or not.
     *
     * After bind() — and fit(), for a trade valued by regression — value()
     * and cashflow() are const and touch no shared state: they are called
     * from several threads at once.
     */
    class FutureValue
    {
      public:
        virtual ~FutureValue() = default;

        /// Dates the grid has to contain: fixings, payments, exercise.
        /// Missing a payment date means missing the jump of the exposure
        /// around it (blueprint §3.3).
        virtual std::vector<Time> event_times() const = 0;

        /// Last date at which the trade can still pay.
        virtual Time maturity() const = 0;

        /// Value today, in closed form.
        virtual Real value_today() const = 0;

        /// Called once by the engine, before any path, with the final grid
        /// (t_1 < ... < t_n; today, t = 0, is not on it).
        /// @throws InvalidInput if one of event_times() is not on the grid.
        virtual void bind(const std::vector<Time> &grid) = 0;

        /// Value at grid[i] of the cash flows **strictly after** grid[i]:
        /// what would have to be replaced if the counterparty defaulted just
        /// after the payment of that date.
        virtual Real value(std::size_t i, const Real *state) const = 0;

        /// Cash flow at grid[i]: > 0 received by the bank, < 0 paid.
        virtual Real cashflow(std::size_t i, const Real *state) const = 0;

        // ── Valuation by regression (lot X4) ────────────────────────────

        /// True when value() is a regression that fit() must estimate first.
        virtual bool needs_pilot() const { return false; }

        /// Called once by the engine, after bind() and before any path of
        /// the main simulation, when needs_pilot().
        virtual void fit(const PilotPaths & /*pilot*/) {}

        /// Most regressors one date can have.
        static constexpr std::size_t kMaxRegressors = quantModeling::kMaxRegressors;

        /**
         * @brief What the value at grid[i] depends on, beyond the trade's
         *        terms: a discrete regime and continuous regressors. A
         *        regression of the trade's future cash flows on the
         *        regressors, regime by regime, estimates value().
         *
         * The regime is what a polynomial cannot follow: a swaption that was
         * exercised is a swap, one that was not is nothing. The regressors
         * are what the value is a smooth function of: the state today, and
         * whatever was fixed earlier and is still to be paid.
         *
         * Defaults: one regime, and the state at grid[i] — a trade whose
         * value is a function of today's state alone.
         */
        virtual std::size_t regime(std::size_t /*i*/, const Real * /*state*/) const { return 0; }

        /// Writes the regressors at grid[i] to `out` (room for
        /// kMaxRegressors) and returns how many.
        virtual std::size_t regressors(std::size_t i, const Real *state, Real *out) const
        {
            out[0] = state[i];
            return 1;
        }
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_FUTURE_VALUE_HPP
