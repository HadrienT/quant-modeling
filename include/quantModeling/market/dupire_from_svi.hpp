#ifndef MARKET_DUPIRE_FROM_SVI_HPP
#define MARKET_DUPIRE_FROM_SVI_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi_surface.hpp"
#include "quantModeling/models/volatility.hpp"

#include <cstddef>

namespace quantModeling
{

    struct DupireFromSVIParams
    {
        Real denom_floor = 0.05;   ///< matches api/app/local_vol/dupire.py's _DENOM_FLOOR
        Real min_local_var = 1e-4; ///< (1% local vol)^2
        Real max_local_var = 16.0; ///< (400% local vol)^2
    };

    /**
     * sigma^2_loc(k, T) via the Gatheral closed-form Dupire formula, evaluated
     * analytically on the SVI surface -- no finite differences anywhere,
     * unlike api/app/local_vol/dupire.py's spline-based version.
     *
     * Returns NaN when the point is numerically unstable: the denominator
     * (svi_density_g_from_variance -- the same g(k) the butterfly check
     * uses) too close to zero, non-positive total variance, or total
     * variance decreasing in T at this k. The interpolated surface should
     * not show that last one if every adjacent slice pair passed
     * svi_slices_are_calendar_arbitrage_free, but it is checked here rather
     * than assumed -- a caller that skipped the check gets a documented
     * failure, not a silently wrong local vol.
     */
    Real svi_surface_local_variance(const SVISurface &surface, Real k, Real T,
                                    const DupireFromSVIParams &params = {}) noexcept;

    /**
     * Builds the (K_grid, T_grid, sigma_loc) grid GridLocalVol needs: T over
     * [surface.ttm_min(), surface.ttm_max()] (there is nothing to extrapolate
     * from beyond the calibrated maturities), strikes over log-moneyness
     * [k_min, k_max] converted at each grid maturity's own forward. Cells
     * where svi_surface_local_variance is unstable are filled from the
     * nearest valid cell -- matching dupire.py's fallback -- but no
     * smoothing pass follows it: dupire.py needed one because its
     * derivatives come from finite differences on a fitted spline, which
     * amplifies noise; these are analytic, so there is no such noise to
     * smooth away.
     */
    GridLocalVol build_local_vol_grid(const SVISurface &surface, Real spot, Real rate, Real dividend,
                                      Real k_min, Real k_max,
                                      std::size_t n_strikes = 100, std::size_t n_maturities = 50,
                                      const DupireFromSVIParams &params = {});

} // namespace quantModeling

#endif
