#ifndef MARKET_SVI_HPP
#define MARKET_SVI_HPP

#include "quantModeling/core/types.hpp"

#include <cstddef>

namespace quantModeling
{

    /**
     * Gatheral's raw SVI parametrisation of one maturity slice's total implied
     * variance as a function of log-moneyness k = ln(K/F_T):
     *
     *   w(k) = a + b * ( rho * (k - m) + sqrt((k - m)^2 + sigma^2) )
     *
     * a       level of the variance minimum
     * b >= 0  angle between the two asymptotes (wing steepness)
     * |rho|<1 rotation: skews the smile left/right
     * m       horizontal translation of the smile's minimum
     * sigma>0 curvature (ATM smoothness) at the minimum
     *
     * Reference: Gatheral, "The Volatility Surface", Wiley 2006, and Gatheral &
     * Jacquier, "Arbitrage-free SVI volatility surfaces", 2014 (the g(k)
     * butterfly condition below).
     */
    struct SVIParams
    {
        Real a = 0.0;
        Real b = 0.1;
        Real rho = 0.0;
        Real m = 0.0;
        Real sigma = 0.1;
    };

    Real svi_total_variance(Real k, const SVIParams &p) noexcept;     ///< w(k)
    Real svi_total_variance_dk(Real k, const SVIParams &p) noexcept;  ///< w'(k)
    Real svi_total_variance_dk2(Real k, const SVIParams &p) noexcept; ///< w''(k)

    /// Implied volatility at log-moneyness k for a slice of maturity T > 0.
    Real svi_implied_vol(Real k, Real T, const SVIParams &p) noexcept;

    /**
     * The Gatheral-Jacquier g(k) function. The risk-neutral density implied by
     * this slice is proportional to g(k) (times a strictly positive factor),
     * so g(k) >= 0 for every k is exactly the butterfly (no static arbitrage)
     * condition for this one slice. Algebraically identical to the Dupire
     * local-variance denominator in api/app/local_vol/dupire.py's
     * _gatheral_local_var -- same formula, same reason it can blow up.
     */
    Real svi_density_g(Real k, const SVIParams &p) noexcept;

    /**
     * Cheap necessary conditions, checked before ever touching a grid:
     * b >= 0, |rho| < 1, sigma > 0, and a + b*sigma*sqrt(1-rho^2) >= 0 (the
     * slice's total variance minimum, attained at k where w'(k) = 0, must be
     * non-negative -- a negative minimum makes some strikes' implied
     * variance negative, which has no real-valued volatility).
     */
    bool svi_satisfies_necessary_conditions(const SVIParams &p) noexcept;

    /**
     * The practical butterfly-arbitrage check this project uses: the
     * necessary conditions above, then g(k) >= 0 sampled on a grid of
     * n_grid points over [k_min, k_max]. Gatheral & Jacquier give no simpler
     * closed-form global certificate for the raw parametrisation, so a grid
     * scan (dense enough to catch a localised dip) is standard practice.
     */
    bool svi_is_butterfly_arbitrage_free(const SVIParams &p, Real k_min, Real k_max,
                                         std::size_t n_grid = 200) noexcept;

} // namespace quantModeling

#endif
