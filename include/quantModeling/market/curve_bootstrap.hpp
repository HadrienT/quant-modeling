#ifndef QM_MARKET_CURVE_BOOTSTRAP_HPP
#define QM_MARKET_CURVE_BOOTSTRAP_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief A money-market deposit quote: a simple (add-on) rate to a single
     *        maturity, no intermediate cash flow.
     *
     * `discount(maturity) = 1 / (1 + rate * maturity)`. `maturity` and `rate`
     * are already on whatever day-count basis the caller wants (this class is
     * convention-agnostic, like DiscountCurve itself) — convert first with a
     * DayCounter, or with t_bill_bond_equivalent_yield() for a T-bill quoted
     * on a discount-rate basis.
     */
    struct DepositQuote
    {
        Time maturity;
        Real rate;
    };

    /**
     * @brief A par-rate instrument: the coupon (or fixed leg) rate that makes
     *        the instrument worth par today, together with its full cash-flow
     *        schedule.
     *
     * Covers two market instruments with the same algebra:
     *  - a **par swap**: `payment_times` are the fixed-leg payment dates,
     *    `accruals` their day-count fractions, `rate` the swap rate;
     *  - a **par bond** (e.g. a Treasury CMT tenor, which is by convention the
     *    yield of a semi-annual bond priced at par): `payment_times` are the
     *    coupon dates, `accruals` the (~0.5y) accrual fractions, `rate` the
     *    coupon / par yield. make_semiannual_bond_quote() builds this schedule
     *    for you.
     *
     * The par condition is `1 = rate · Σ accrual_i · DF(payment_i) + DF(last)`
     * — solved for the discount factor at `payment_times.back()` given the
     * curve already bootstrapped up to the previous pillar.
     */
    struct ParRateQuote
    {
        Time maturity() const { return payment_times.back(); }

        Real rate;
        std::vector<Time> payment_times; // strictly increasing, ends at maturity
        std::vector<Real> accruals;      // same size as payment_times
    };

    /// The standard Treasury CMT convention: a semi-annual coupon bond priced
    /// at par, coupon dates at 0.5y, 1.0y, … up to `maturity` (assumed a whole
    /// number of half-years).
    ParRateQuote make_semiannual_bond_quote(Time maturity, Real par_yield);

    /**
     * @brief Bootstrap a DiscountCurve from money-market deposits and
     *        par-rate instruments (blueprint: etc/roadmap.md, chantier 0).
     *
     * The classic desk algorithm: sort every quote by maturity, then solve for
     * one new discount factor at a time, always against the curve already
     * built from shorter pillars —
     *  - a deposit solves in closed form;
     *  - a par-rate instrument solves by bisection on the zero rate at its
     *    maturity, because its schedule may have coupon dates inside the
     *    segment being solved (e.g. a 5Y quote following a 3Y pillar has
     *    coupons at 3.5Y, 4Y, 4.5Y with no pillar of their own yet): those are
     *    priced by log-discount-factor interpolation on the *candidate* curve
     *    at each trial root, exactly the interpolation DiscountCurve itself
     *    uses, so a repriced input instrument matches its quote once solved
     *    (this is the property the tests check, not merely that DFs decrease).
     *
     * @throws InvalidInput on an empty quote set, a non-positive maturity, a
     *         duplicate maturity, or if bisection fails to bracket a root
     *         (a quote priced far outside what an arbitrage-free curve can
     *         produce from what came before it).
     */
    DiscountCurve bootstrap_curve(const std::vector<DepositQuote> &deposits,
                                  const std::vector<ParRateQuote> &par_quotes);

    /**
     * @brief Convert a T-bill secondary-market discount rate to the
     *        equivalent simple (bond-equivalent, add-on) yield.
     *
     * `d` is the quoted discount rate (Actual/360); `days` the actual days to
     * maturity. Standard formula for `days <= 182`:
     *   r = 365 · d / (360 − d · days)
     * (the longer-maturity compounding correction is out of scope — CMT
     * quotes already give the > 6M points).
     */
    Real t_bill_bond_equivalent_yield(Real discount_rate, int days);

} // namespace quantModeling

#endif // QM_MARKET_CURVE_BOOTSTRAP_HPP
