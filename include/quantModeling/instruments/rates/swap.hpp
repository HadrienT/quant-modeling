#ifndef INSTRUMENT_RATES_SWAP_HPP
#define INSTRUMENT_RATES_SWAP_HPP

#include "quantModeling/instruments/base.hpp"

#include <utility>
#include <vector>

namespace quantModeling
{

    /**
     * @brief One accrual period of a swap leg, on the Time axis (year
     *        fractions from the valuation date).
     *
     * `accrual` is the day-count fraction of [start, end] under the leg's
     * convention; on a floating leg [start, end] is also the index period
     * whose forward rate sets the coupon (blueprint/wp/21-rates.md §2).
     */
    struct CouponPeriod
    {
        Time start;
        Time end;
        Time payment;
        Real accrual;
    };

    /**
     * @brief A vanilla fixed-for-floating interest-rate swap (Andersen &
     *        Piterbarg 2010, §5.5).
     *
     * Fixed leg: `fixed_rate · accrual · notional` at each fixed payment.
     * Floating leg: `(F_j + spread) · accrual · notional`, F_j the simple
     * forward of the projection curve over the coupon's [start, end]. The
     * swap is a payer when it pays fixed. Instruments read no market data:
     * the discount and projection curves come with the engine (multi-curve,
     * engines/analytic/swap.hpp).
     */
    struct InterestRateSwap final : Instrument
    {
        std::vector<CouponPeriod> fixed_leg;
        std::vector<CouponPeriod> floating_leg;
        Real fixed_rate = 0.0;
        Real spread = 0.0; ///< over the floating index
        Real notional = 1.0;
        bool payer = true; ///< pays fixed, receives floating

        Time start() const;
        Time maturity() const;

        /// The coupons that start on or after `t`: what exercising a swaption
        /// at `t` enters into.
        InterestRateSwap tail_from(Time t) const;

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    /**
     * @brief A spot- or forward-starting swap on a regular Time grid:
     *        `fixed_frequency` / `float_frequency` payments a year from
     *        `start` to `start + tenor`, accruals the period lengths
     *        (an ACT/ACT-like basis on the Time axis, as make_cds). Payment
     *        on the period end.
     * @throws InvalidInput unless tenor · frequency is a whole number for
     *         both legs.
     */
    InterestRateSwap make_swap(Time start, Time tenor, Real fixed_rate, int fixed_frequency = 1,
                               int float_frequency = 4, Real notional = 1.0, bool payer = true,
                               Real spread = 0.0);

    /**
     * @brief A European swaption, physically settled: the right, at
     *        `expiry`, to enter `swap` (a payer swaption when swap.payer).
     *        The swap starts on or after the expiry.
     */
    struct Swaption final : Instrument
    {
        InterestRateSwap swap;
        Time expiry;

        Swaption(InterestRateSwap swap_, Time expiry_) : swap(std::move(swap_)), expiry(expiry_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

    /**
     * @brief A Bermudan swaption: the right, on any one of
     *        `exercise_times`, to enter the coupons of `swap` that start on or
     *        after that date (the co-terminal swap). Exercise dates are
     *        usually the fixed-leg start dates.
     */
    struct BermudanSwaption final : Instrument
    {
        InterestRateSwap swap;
        std::vector<Time> exercise_times; ///< strictly increasing, > 0

        BermudanSwaption(InterestRateSwap swap_, std::vector<Time> exercise_times_)
            : swap(std::move(swap_)), exercise_times(std::move(exercise_times_))
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
