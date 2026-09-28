#include "quantModeling/market/vol_surface_pipeline.hpp"

#include "quantModeling/market/svi_surface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace quantModeling
{

    std::vector<std::size_t> select_surface_slices(const std::vector<SVISliceCalibration> &slices,
                                                   const std::vector<std::size_t> &n_quotes, Real k_min,
                                                   Real k_max, const SurfaceSliceSelection &selection)
    {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < slices.size(); ++i)
            if (slices[i].ttm >= selection.min_ttm)
                candidates.push_back(i);
        if (candidates.size() < 2)
        {
            candidates.resize(slices.size());
            for (std::size_t i = 0; i < slices.size(); ++i)
                candidates[i] = i;
        }

        // Longest path in the DAG whose edges are the admissible consecutive
        // pairs (candidates are in maturity order): the most slices, then
        // the most quotes. Slices first, because each one is a pillar of the
        // term structure; quotes only break ties, as a slice with many
        // quotes is not more right about its level than its neighbours (on
        // SPY, weighting by quotes kept a 150-quote slice two vol points
        // above the others and dropped the three that followed it).
        using Score = std::pair<std::size_t, std::size_t>; // (slices, quotes)
        const std::size_t n = candidates.size();
        std::vector<Score> best(n);
        std::vector<std::size_t> previous(n, n);
        for (std::size_t j = 0; j < n; ++j)
        {
            const SVISliceCalibration &longer = slices[candidates[j]];
            const std::size_t quotes = n_quotes[candidates[j]];
            best[j] = {1, quotes};
            for (std::size_t i = 0; i < j; ++i)
            {
                const SVISliceCalibration &shorter = slices[candidates[i]];
                const Score through{best[i].first + 1, best[i].second + quotes};
                if (through <= best[j] || longer.ttm < (1.0 + selection.min_relative_gap) * shorter.ttm ||
                    !svi_slices_are_calendar_arbitrage_free(shorter, longer, k_min, k_max))
                    continue;
                best[j] = through;
                previous[j] = i;
            }
        }
        std::size_t last = 0;
        for (std::size_t j = 1; j < n; ++j)
            if (best[j] > best[last])
                last = j;
        std::vector<std::size_t> chain;
        for (std::size_t j = last; j != n; j = previous[j])
            chain.push_back(candidates[j]);
        std::reverse(chain.begin(), chain.end());
        return chain.size() >= 2 ? chain : candidates;
    }

    VolSurfacePipelineResult calibrate_vol_surface(
        std::vector<RawOptionQuote> raw_quotes, Real spot, Real rate, Real dividend,
        Real k_min, Real k_max,
        std::size_t n_strikes, std::size_t n_maturities,
        std::size_t min_quotes_per_slice,
        const CleaningParams &cleaning_params,
        const calibration::LevenbergMarquardtSettings &lm_settings,
        const DupireFromSVIParams &dupire_params,
        const SurfaceSliceSelection &selection)
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
            report.quotes = std::move(slice_quotes);

            calibrations.push_back(std::move(calibration));
            reports.push_back(std::move(report));
        }

        // Intersection of the log-moneyness ranges the slices of at least
        // min_ttm actually have quotes over, each out to its reach. A fixed
        // k range that looks reasonable for a one-year slice can be a wild
        // extrapolation for a one-week one: svi_implied_vol = sqrt(w(k)/T)
        // divides by a tiny T, so whatever wing curvature exists at the edge
        // of the requested range is massively amplified on the shortest
        // maturity. Found against a real AAPL chain (a ~6-day slice implying
        // ~600% vol at k=0.6), not hypothesised. The slices below min_ttm
        // stay out of the surface altogether, and a slice quoted past its
        // reach does not narrow the grid -- the few-day slices, quoted over
        // a few percent of moneyness, used to shrink the whole grid to it.
        const auto long_enough = std::count_if(maturities.begin(), maturities.end(),
                                               [&](Real t)
                                               { return t >= selection.min_ttm; });
        const Real range_ttm = long_enough >= 2 ? selection.min_ttm : -std::numeric_limits<Real>::infinity();
        Real observed_k_min = -std::numeric_limits<Real>::infinity();
        Real observed_k_max = std::numeric_limits<Real>::infinity();
        for (std::size_t i = 0; i < reports.size(); ++i)
        {
            const auto &report = reports[i];
            if (report.quotes.empty() || report.ttm < range_ttm)
                continue;
            const auto [lo, hi] = std::minmax_element(report.quotes.begin(), report.quotes.end(),
                                                      [](const SVISliceQuote &a, const SVISliceQuote &b)
                                                      { return a.log_moneyness < b.log_moneyness; });
            const Real reach =
                selection.reach_std_devs * std::sqrt(std::max(svi_total_variance(0.0, calibrations[i].params), 0.0));
            if (lo->log_moneyness > -reach)
                observed_k_min = std::max(observed_k_min, lo->log_moneyness);
            if (hi->log_moneyness < reach)
                observed_k_max = std::min(observed_k_max, hi->log_moneyness);
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

        std::vector<std::size_t> n_quotes;
        for (const auto &report : reports)
            n_quotes.push_back(report.n_quotes);
        std::vector<SVISliceCalibration> kept;
        for (const std::size_t i : select_surface_slices(calibrations, n_quotes, k_min, k_max, selection))
        {
            reports[i].in_surface = true;
            kept.push_back(calibrations[i]);
        }

        SVISurface surface(std::move(kept));
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
