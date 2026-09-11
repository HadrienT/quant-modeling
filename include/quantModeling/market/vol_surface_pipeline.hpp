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
    };

    /**
     * The full pipeline, raw quotes in, a local-vol grid out: clean
     * (RawVolSurface), calibrate one SVI slice per maturity with enough
     * cleaned quotes (calibrate_svi_slice), interpolate into a surface
     * (SVISurface), build the grid (build_local_vol_grid).
     *
     * Throws InvalidInput if fewer than 2 maturities end up with at least
     * min_quotes_per_slice cleaned quotes -- SVISurface needs at least 2
     * slices, and a single slice is not a surface.
     */
    VolSurfacePipelineResult calibrate_vol_surface(
        std::vector<RawOptionQuote> raw_quotes, Real spot, Real rate, Real dividend,
        Real k_min = -0.6, Real k_max = 0.6,
        std::size_t n_strikes = 100, std::size_t n_maturities = 50,
        std::size_t min_quotes_per_slice = 6,
        const CleaningParams &cleaning_params = {},
        const calibration::LevenbergMarquardtSettings &lm_settings = {},
        const DupireFromSVIParams &dupire_params = {});

} // namespace quantModeling

#endif
