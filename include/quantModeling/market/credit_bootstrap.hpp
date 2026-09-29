#ifndef QM_MARKET_CREDIT_BOOTSTRAP_HPP
#define QM_MARKET_CREDIT_BOOTSTRAP_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/discount_curve.hpp"

#include <vector>

namespace quantModeling
{

    /// A par CDS spread for one maturity (decimal: 0.01 = 100bp).
    struct CdsQuote
    {
        Time maturity;
        Real spread;
    };

    /**
     * @brief Bootstrap a piecewise-constant hazard curve from par CDS spreads
     *        (blueprint: etc/roadmap.md, chantier 4b; O'Kane 2008, §7.3).
     *
     * Quotes are sorted by maturity; the hazard rate on (T_{i-1}, T_i] is
     * the one that makes a CDS of maturity T_i, paying `frequency` times a
     * year (make_cds), worth zero at its quoted spread — solved by bisection
     * with every earlier segment held fixed. The par spread increases with
     * the hazard on the new segment, so a root is unique when it exists.
     *
     * @throws InvalidInput on no quote, a non-positive maturity or spread, a
     *         duplicate maturity, or a quote no non-negative hazard reaches:
     *         a spread curve that falls too steeply implies a survival
     *         probability that rises, which is an arbitrage — reported, not
     *         floored away.
     */
    CreditCurve bootstrap_credit_curve(std::vector<CdsQuote> quotes, const DiscountCurve &discount,
                                       Real recovery, int frequency = 4);

    /// The flat hazard that reprices a single par spread; ≈ spread / (1 - R),
    /// the "credit triangle", which it tends to as payments become continuous.
    Real flat_hazard_from_spread(Time maturity, Real spread, const DiscountCurve &discount,
                                 Real recovery, int frequency = 4);

} // namespace quantModeling

#endif // QM_MARKET_CREDIT_BOOTSTRAP_HPP
