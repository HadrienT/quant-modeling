#include "quantModeling/market/vol_surface_pipeline.hpp"

#include "quantModeling/market/svi_surface.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace quantModeling
{

    VolSurfacePipelineResult calibrate_vol_surface(
        std::vector<RawOptionQuote> raw_quotes, Real spot, Real rate, Real dividend,
        Real k_min, Real k_max,
        std::size_t n_strikes, std::size_t n_maturities,
        std::size_t min_quotes_per_slice,
        const CleaningParams &cleaning_params,
        const calibration::LevenbergMarquardtSettings &lm_settings,
        const DupireFromSVIParams &dupire_params)
    {
        RawVolSurface raw_surface(std::move(raw_quotes), spot, rate, dividend, cleaning_params);

        std::map<Real, std::size_t> quotes_per_ttm;
        for (const auto &q : raw_surface.quotes())
            ++quotes_per_ttm[q.ttm];

        std::vector<Real> maturities;
        for (const auto &[ttm, count] : quotes_per_ttm)
            if (count >= min_quotes_per_slice)
                maturities.push_back(ttm);

        if (maturities.size() < 2)
            throw InvalidInput(
                "calibrate_vol_surface: fewer than 2 maturities have at least "
                "min_quotes_per_slice cleaned quotes -- cannot build a surface");

        std::vector<SVISliceCalibration> calibrations;
        std::vector<VolSurfaceSliceReport> reports;
        calibrations.reserve(maturities.size());
        reports.reserve(maturities.size());

        // Intersection, across every slice, of the log-moneyness range that
        // slice actually has quotes over. A fixed k range that looks
        // reasonable for a one-year slice can be a wild extrapolation for a
        // one-week one: svi_implied_vol = sqrt(w(k)/T) divides by a tiny T,
        // so whatever wing curvature exists at the edge of the requested
        // range is massively amplified on the shortest maturity. Found
        // against a real AAPL chain (a ~6-day slice implying ~600% vol at
        // k=0.6), not hypothesised. Clamping to what every slice actually
        // observed guarantees no slice is ever asked to extrapolate beyond
        // its own data.
        Real observed_k_min = -std::numeric_limits<Real>::infinity();
        Real observed_k_max = std::numeric_limits<Real>::infinity();

        for (const Real ttm : maturities)
        {
            std::vector<SVISliceQuote> slice_quotes = svi_quotes_from_raw_surface(raw_surface, ttm);
            SVISliceCalibration calibration = calibrate_svi_slice(slice_quotes, ttm, lm_settings);

            VolSurfaceSliceReport report;
            report.ttm = ttm;
            report.params = calibration.params;
            report.rmse = calibration.report.rmse;
            report.worst_residual = calibration.report.worst_residual;
            report.n_quotes = slice_quotes.size();
            report.iterations = calibration.report.iterations;
            report.converged = calibration.report.converged;
            report.butterfly_arbitrage_free = calibration.butterfly_arbitrage_free;

            calibrations.push_back(std::move(calibration));
            reports.push_back(report);

            if (!slice_quotes.empty())
            {
                Real slice_k_min = slice_quotes.front().log_moneyness;
                Real slice_k_max = slice_k_min;
                for (const auto &sq : slice_quotes)
                {
                    slice_k_min = std::min(slice_k_min, sq.log_moneyness);
                    slice_k_max = std::max(slice_k_max, sq.log_moneyness);
                }
                observed_k_min = std::max(observed_k_min, slice_k_min);
                observed_k_max = std::min(observed_k_max, slice_k_max);
            }
        }

        // Clamp the caller's requested range to the observed intersection,
        // unless that intersection is degenerate (e.g. disjoint strike
        // ranges across maturities) -- a real chain always has every slice
        // quoted around the current spot, so this is a safety net, not the
        // expected path.
        if (observed_k_max > observed_k_min)
        {
            k_min = std::max(k_min, observed_k_min);
            k_max = std::min(k_max, observed_k_max);
        }

        std::vector<bool> calendar_ok(calibrations.size() - 1);
        for (std::size_t i = 0; i + 1 < calibrations.size(); ++i)
            calendar_ok[i] = svi_slices_are_calendar_arbitrage_free(
                calibrations[i], calibrations[i + 1], k_min, k_max);

        SVISurface surface(std::move(calibrations));
        GridLocalVol grid = build_local_vol_grid(
            surface, spot, rate, dividend, k_min, k_max, n_strikes, n_maturities, dupire_params);

        VolSurfacePipelineResult result;
        result.cleaning_stats = raw_surface.stats();
        result.slices = std::move(reports);
        result.calendar_arbitrage_free = std::move(calendar_ok);
        result.K_grid = grid.K_grid();
        result.T_grid = grid.T_grid();
        result.sigma_loc = grid.sigma_loc();
        result.k_min = k_min;
        result.k_max = k_max;
        return result;
    }

} // namespace quantModeling
