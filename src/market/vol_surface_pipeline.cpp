#include "quantModeling/market/vol_surface_pipeline.hpp"

#include "quantModeling/market/svi_surface.hpp"

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
        return result;
    }

} // namespace quantModeling
