#ifndef QM_MARKET_MULTI_CURVE_BOOTSTRAP_HPP
#define QM_MARKET_MULTI_CURVE_BOOTSTRAP_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/rates/swap.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/discount_curve.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief A par OIS swap: fixed rate against the compounded overnight
     *        rate, annual payments on both legs.
     *
     * The compounded overnight leg of a spot-starting OIS is worth
     * P(0) − P(T) on its own discount curve (the index and the collateral
     * rate are the same), so a par OIS obeys the par-bond algebra of
     * ParRateQuote exactly and the OIS curve bootstraps with
     * bootstrap_curve() (blueprint/wp/21-rates.md §1). Tenors under a year
     * pay once, at maturity.
     */
    ParRateQuote make_ois_quote(Time maturity, Real rate);

    /// A simple forward rate on the projection index over [start, end]: a
    /// FRA, or the index's own fixing when start = 0.
    struct FraQuote
    {
        Time start;
        Time end;
        Real rate;
        Real accrual; ///< day-count fraction of [start, end]
    };

    /// A par swap on the projection index (e.g. fixed vs 3M), discounted on
    /// the OIS curve: `rate` is its par fixed rate.
    struct ProjectionSwapQuote
    {
        InterestRateSwap swap; ///< fixed_rate is the quote

        Time maturity() const { return swap.maturity(); }
    };

    ProjectionSwapQuote make_projection_swap_quote(Time tenor, Real rate, int fixed_frequency = 1,
                                                   int float_frequency = 4);

    /**
     * @brief Bootstrap the projection curve of a floating index from FRAs
     *        and par swaps, **given** the OIS discount curve (the
     *        post-2008 multi-curve set-up, Ametrano & Bianchetti 2013).
     *
     * One pillar per quote at its last index date, sorted by maturity;
     * each is solved by bisection on its zero rate with every earlier
     * pillar fixed and the dates inside the new segment priced by the
     * interpolation DiscountCurve itself uses (log-linear), so the curve
     * reprices every input to the bisection tolerance — the property the
     * tests check. With the projection curve equal to the discount curve
     * this is the single-curve bootstrap.
     *
     * The curve extrapolates at a flat forward (CurveExtrapolation), so a
     * swap's first coupons before the first pillar keep their forward.
     * The projection curve is a pseudo-discount curve: only ratios
     * P(start)/P(end) on it mean anything (forwards), never a present value.
     *
     * @throws InvalidInput on no quotes, a duplicate maturity, or a quote no
     *         zero rate in [-100 %, 300 %] reaches.
     */
    DiscountCurve bootstrap_projection_curve(const DiscountCurve &discount, std::vector<FraQuote> fras,
                                             std::vector<ProjectionSwapQuote> swaps);

} // namespace quantModeling

#endif // QM_MARKET_MULTI_CURVE_BOOTSTRAP_HPP
