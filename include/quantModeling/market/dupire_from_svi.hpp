#ifndef MARKET_DUPIRE_FROM_SVI_HPP
#define MARKET_DUPIRE_FROM_SVI_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi_calibration.hpp"
#include "quantModeling/market/svi_surface.hpp"
#include "quantModeling/models/volatility.hpp"

#include <cstddef>
#include <vector>

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
     * The maturity columns of the local-vol grid: n_maturities points on
     * (0, surface.ttm_max()], aligned on the slices. Linear total variance
     * makes dw/dT, and so the local vol, jump at every slice; each segment
     * [T_{i-1}, T_i] gets a column at each edge -- T_i itself, and
     * T_{i-1} (1 + 1e-9) for the right limit -- so GridLocalVol's linear
     * interpolation in T carries the jump exactly instead of blending two
     * segments over a column. The other columns go to the segments in
     * proportion to their length. A column straddling a slice gives the
     * superbucket (market/superbucket.hpp) of a one-year call vegas of
     * opposite signs to short slices, where Dupire gives it none. Equal
     * steps when n_maturities is below two per segment.
     */
    std::vector<Real> local_vol_maturities(const SVISurface &surface, std::size_t n_maturities);

    /**
     * Builds the (K_grid, T_grid, sigma_loc) grid GridLocalVol needs: T on
     * local_vol_maturities (the surface grows from zero variance before its
     * first slice, and there is nothing to extrapolate from beyond its
     * last), strikes over log-moneyness
     * [k_min, k_max] converted at each grid maturity's own forward. Cells
     * where svi_surface_local_variance is unstable are filled from the
     * nearest valid cell -- matching dupire.py's fallback -- but no
     * smoothing pass follows it: dupire.py needed one because its
     * derivatives come from finite differences on a fitted spline, which
     * amplifies noise; these are analytic, so there is no such noise to
     * smooth away.
     */
    /// A log-moneyness interval, k = ln(K / F_T).
    struct QuotedBand
    {
        Real lo = 0.0;
        Real hi = 0.0;
    };

    /**
     * The band a slice's SVI is trusted on (blueprint/wp/26-performance.md
     * §9.3): its quotes' log-moneyness range, clamped to [k_min, k_max]. Past
     * it the SVI wing is an extrapolation, which svi_implied_vol = sqrt(w/T)
     * amplifies on a short slice.
     */
    QuotedBand quoted_band(const std::vector<SVISliceQuote> &quotes, Real k_min, Real k_max);

    /**
     * Each grid maturity's band: the intersection of the bands of the two
     * slices T falls between (the first slice's before it, the last one's
     * beyond), so that a local vol is only computed where both slices whose
     * total variances it interpolates are quoted. `slice_bands` follows
     * surface.slices().
     */
    std::vector<QuotedBand> local_vol_column_bands(const SVISurface &surface,
                                                   const std::vector<QuotedBand> &slice_bands,
                                                   const std::vector<Real> &T_grid);

    /// The cells (K-major) outside their column's band.
    std::vector<bool> outside_column_bands(const std::vector<Real> &K_grid, const std::vector<Real> &T_grid,
                                           Real spot, Real rate, Real dividend,
                                           const std::vector<QuotedBand> &column);

    /**
     * The fill of a banded grid: a cell outside its band, or where Dupire is
     * unstable, takes its column's nearest valid cell along K, so the local
     * vol is flat in the strike past what a maturity supports instead of
     * borrowed from another maturity (the ring search of
     * nearest_valid_sources() copied two-year call-wing cells from short
     * maturities on SPY, local vols of 50-110 %). A column with no valid
     * cell keeps the ring search. The superbucket replays the same map.
     */
    std::vector<std::size_t> banded_sources(const std::vector<Real> &grid, std::size_t n_strikes,
                                            std::size_t n_maturities, const std::vector<bool> &outside);

    /**
     * ... and with `slice_bands` (one per surface slice, quoted_band()), a
     * band per grid maturity (local_vol_column_bands()) instead of one for
     * the whole grid: a cell outside its column's band is a gap, filled
     * from its column's nearest in-band cell along K (banded_sources()). A short
     * maturity, quoted near the money only, then no longer caps the strikes
     * of the long ones (WP 26 §9.3: SPY's three-week slice, quoted to +8 %,
     * capped the whole two-year grid there).
     */
    GridLocalVol build_local_vol_grid(const SVISurface &surface, Real spot, Real rate, Real dividend,
                                      Real k_min, Real k_max,
                                      std::size_t n_strikes = 100, std::size_t n_maturities = 50,
                                      const DupireFromSVIParams &params = {},
                                      const std::vector<QuotedBand> *slice_bands = nullptr);

    /// No valid cell anywhere on the grid to copy from.
    inline constexpr std::size_t kNoSource = static_cast<std::size_t>(-1);

    /**
     * For each cell of a K-major (n_strikes x n_maturities) grid, the cell
     * whose value it takes: itself where the value is not NaN, else the
     * nearest valid cell in grid-index space (a ring search outward), or
     * kNoSource if there is none. build_local_vol_grid fills the gaps of the
     * Dupire formula this way; the superbucket (market/superbucket.hpp)
     * replays the same map on its tape.
     */
    std::vector<std::size_t> nearest_valid_sources(const std::vector<Real> &grid,
                                                   std::size_t n_strikes,
                                                   std::size_t n_maturities);

} // namespace quantModeling

#endif
