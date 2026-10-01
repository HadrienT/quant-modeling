#ifndef QM_ENGINES_XVA_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_FUTURE_VALUE_HPP

#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

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
     * After bind(), value() and cashflow() are const and touch no shared
     * state: they are called from several threads at once.
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
    };

} // namespace quantModeling

#endif // QM_ENGINES_XVA_FUTURE_VALUE_HPP
