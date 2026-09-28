#ifndef MARKET_VOL_SURFACE_PIPELINE_HPP
#define MARKET_VOL_SURFACE_PIPELINE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/raw_vol_surface.hpp"
#include "quantModeling/market/svi.hpp"
#include "quantModeling/market/svi_calibration.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /// Per-maturity calibration diagnostics -- the raw material for the
    /// calibration-report screen (blueprint/wp/15-future-quant-surfaces.md §1:
    /// RMSE in vol points, worst point, iterations, arbitrage status).
    struct VolSurfaceSliceReport
    {
        Real ttm = 0.0;
        SVIParams params;
        Real rmse = 0.0;           ///< implied-vol points
        Real worst_residual = 0.0; ///< implied-vol points
        std::size_t n_quotes = 0;
        std::size_t iterations = 0;
        bool converged = false;
        bool butterfly_arbitrage_free = false;
        /// Whether the slice is one of the maturities the local-vol grid is
        /// built from (see select_surface_slices). A slice left out is still
        /// calibrated and reported, for display and for Heston's fit.
        bool in_surface = false;
        /// The cleaned quotes the slice was fitted to (log-moneyness, implied
        /// vol, weight): the market the superbucket differentiates against.
        std::vector<SVISliceQuote> quotes;
    };

    struct VolSurfacePipelineResult
    {
        CleaningStats cleaning_stats;
        std::vector<VolSurfaceSliceReport> slices; ///< sorted by ttm

        /// calendar_arbitrage_free[i] holds for the pair (slices[i], slices[i+1]);
        /// size is slices.size() - 1.
        std::vector<bool> calendar_arbitrage_free;

        /// The local-vol grid, K-major: sigma_loc[i*T_grid.size()+j] =
        /// sigma_loc(K_grid[i], T_grid[j]) -- exactly the layout
        /// quantmodeling's existing LocalVolInput (K_grid/T_grid/sigma_loc_flat)
        /// expects, unchanged.
        std::vector<Real> K_grid;
        std::vector<Real> T_grid;
        std::vector<Real> sigma_loc;

        /// The log-moneyness range actually used to build the grid above --
        /// the caller's requested [k_min, k_max], clamped to the
        /// intersection of what every slice of at least
        /// SurfaceSliceSelection::min_ttm actually had quotes over, each
        /// within its reach (SurfaceSliceSelection::reach_std_devs; see
        /// calibrate_vol_surface's doc comment). Any other display
        /// grid built from `slices` (e.g. an implied-vol surface evaluated
        /// in Python) should reuse this range rather than its own guess, so
        /// the two surfaces never disagree about how far to extrapolate.
        Real k_min = 0.0;
        Real k_max = 0.0;
    };

    /// Which calibrated slices the local-vol surface is built from.
    struct SurfaceSliceSelection
    {
        /// Slices shorter than this (in years) neither set the strike range
        /// nor enter the surface. A few-day slice is quoted over a few
        /// percent of moneyness, so the intersection of every slice's range
        /// used to shrink the whole grid to it (+/-7 % on SPY, with a -15 %
        /// barrier priced on flat-extrapolated local vol). The grid starts
        /// at the first kept slice; GridLocalVol holds its first column flat
        /// before it.
        Real min_ttm = 0.05;
        /// Consecutive surface slices are at least this far apart,
        /// T_next >= (1 + min_relative_gap) T. Linear total variance makes
        /// dw/dT the slope between two slices, so its error is about twice
        /// the fit error over T_next - T: two slices 0.02y apart turned
        /// fit noise of a vol point into local vols of 1 % or 70 %.
        Real min_relative_gap = 0.25;
        /// A slice narrows the grid's strike range only within its reach:
        /// this many standard deviations of log(S_T / F_T), reach_std_devs
        /// sqrt(w(0)). Past it, paths to that maturity (almost) never go, and
        /// the slice's SVI wing is extrapolated there instead. On SPY, the
        /// 4-week slice, quoted to +10 %, capped the call side of the whole
        /// grid at +10 %, which a one-year call crosses at 0.8 of its
        /// standard deviation.
        Real reach_std_devs = 3.0;
    };

    /**
     * The slices the surface is built from, as indices into `slices` (sorted
     * by ttm): among the candidates (ttm >= min_ttm), the chain with the
     * most slices (then the most quotes) in which every consecutive pair is calendar-arbitrage-free
     * on [k_min, k_max] and spaced by min_relative_gap -- a longest path in
     * the DAG of admissible pairs, O(n^2) calendar checks.
     *
     * Independent SVI fits of a real chain are not calendar-consistent: on
     * SPY, the at-the-money total variance of consecutive long slices goes
     * up and down by a third, and Dupire turns every decrease into a
     * negative local variance. The chain keeps the maturities that agree
     * with each other and leaves out the ones that contradict them. Linear
     * interpolation in T between two calendar-free slices is itself
     * calendar-free, so every column of the grid has dw/dT >= 0.
     *
     * Falls back to every candidate (or every slice, when fewer than two
     * reach min_ttm) if no chain of two exists, so a pathological chain
     * still yields a grid -- with the nearest-valid fill doing the rest, as
     * before.
     */
    std::vector<std::size_t> select_surface_slices(const std::vector<SVISliceCalibration> &slices,
                                                   const std::vector<std::size_t> &n_quotes, Real k_min,
                                                   Real k_max, const SurfaceSliceSelection &selection = {});

    /**
     * The full pipeline, raw quotes in, a local-vol grid out: clean
     * (RawVolSurface), calibrate one SVI slice per maturity with enough
     * cleaned quotes (calibrate_svi_slice), interpolate into a surface
     * (SVISurface), build the grid (build_local_vol_grid).
     *
     * Throws InvalidInput if fewer than 2 maturities end up with at least
     * min_quotes_per_slice cleaned quotes -- SVISurface needs at least 2
     * slices, and a single slice is not a surface.
     *
     * k_min/k_max are a request, not a guarantee: they are clamped to the
     * intersection of the observed log-moneyness ranges of the slices of at
     * least selection.min_ttm, each within its reach, before the grid is
     * built (see
     * VolSurfacePipelineResult::k_min), and the surface is built from the
     * slices select_surface_slices keeps.
     * Without this, a fixed range wide enough to look reasonable on a
     * one-year slice can force a one-week slice's SVI wing to extrapolate
     * far past its data -- svi_implied_vol divides total variance by T, so
     * the same extrapolated wing curvature that is a small effect at T=1
     * becomes a large one at T=1/52.
     */
    VolSurfacePipelineResult calibrate_vol_surface(
        std::vector<RawOptionQuote> raw_quotes, Real spot, Real rate, Real dividend,
        Real k_min = -0.6, Real k_max = 0.6,
        std::size_t n_strikes = 100, std::size_t n_maturities = 50,
        std::size_t min_quotes_per_slice = 6,
        const CleaningParams &cleaning_params = {},
        const calibration::LevenbergMarquardtSettings &lm_settings = {},
        const DupireFromSVIParams &dupire_params = {},
        const SurfaceSliceSelection &selection = {});

} // namespace quantModeling

#endif
