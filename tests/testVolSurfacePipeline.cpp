#include <gtest/gtest.h>

#include "quantModeling/market/vol_surface_pipeline.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        Real bs_call_price(Real S, Real K, Real T, Real r, Real sigma)
        {
            const Real vol_sqrt_t = sigma * std::sqrt(T);
            const Real d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / vol_sqrt_t;
            const Real d2 = d1 - vol_sqrt_t;
            return S * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d2);
        }

        std::vector<RawOptionQuote> synthetic_slice(Real spot, Real ttm, const SVIParams &params)
        {
            std::vector<RawOptionQuote> raw;
            for (Real strike = 75.0; strike <= 130.0; strike += 2.5)
            {
                const Real k = std::log(strike / spot);
                const Real iv = svi_implied_vol(k, ttm, params);
                const Real price = bs_call_price(spot, strike, ttm, 0.0, iv);

                RawOptionQuote q;
                q.strike = strike;
                q.ttm = ttm;
                q.is_call = true;
                q.bid = price - 0.001;
                q.ask = price + 0.001;
                q.last = price;
                q.volume = 500;
                q.open_interest = 1000;
                q.implied_vol = iv;
                q.has_iv = true;
                raw.push_back(q);
            }
            return raw;
        }

        SVIParams smile(Real a)
        {
            return SVIParams{a, 0.10, -0.40, 0.0, 0.30};
        }

        std::vector<RawOptionQuote> two_maturity_chain(Real spot)
        {
            std::vector<RawOptionQuote> raw = synthetic_slice(spot, 0.25, smile(0.02));
            const std::vector<RawOptionQuote> raw_1y = synthetic_slice(spot, 1.00, smile(0.10));
            raw.insert(raw.end(), raw_1y.begin(), raw_1y.end());
            return raw;
        }

    } // namespace

    TEST(VolSurfacePipeline, ThrowsWithFewerThanTwoUsableMaturities)
    {
        const std::vector<RawOptionQuote> raw = synthetic_slice(100.0, 0.5, smile(0.04)); // one maturity only
        EXPECT_THROW(calibrate_vol_surface(raw, 100.0, 0.0, 0.0), InvalidInput);
    }

    TEST(VolSurfacePipeline, ProducesOneReportPerUsableMaturityAndAMatchingGrid)
    {
        const std::vector<RawOptionQuote> raw = two_maturity_chain(100.0);
        const auto result = calibrate_vol_surface(raw, 100.0, 0.0, 0.0, -0.6, 0.6, 40, 20);

        EXPECT_EQ(result.cleaning_stats.raw_count, raw.size());
        ASSERT_EQ(result.slices.size(), 2u);
        EXPECT_NEAR(result.slices[0].ttm, 0.25, 1e-9);
        EXPECT_NEAR(result.slices[1].ttm, 1.00, 1e-9);
        EXPECT_TRUE(result.slices[0].converged);
        EXPECT_TRUE(result.slices[1].converged);
        EXPECT_LT(result.slices[0].rmse, 1e-2);
        EXPECT_LT(result.slices[1].rmse, 1e-2);

        ASSERT_EQ(result.calendar_arbitrage_free.size(), 1u);
        EXPECT_TRUE(result.calendar_arbitrage_free[0]);

        EXPECT_EQ(result.K_grid.size(), 40u);
        EXPECT_EQ(result.T_grid.size(), 20u);
        EXPECT_EQ(result.sigma_loc.size(), 40u * 20u);
    }

    TEST(VolSurfacePipeline, EveryGridCellIsFiniteAndPositive)
    {
        const std::vector<RawOptionQuote> raw = two_maturity_chain(100.0);
        const auto result = calibrate_vol_surface(raw, 100.0, 0.02, 0.0, -0.6, 0.6, 30, 15);

        for (const Real sigma : result.sigma_loc)
        {
            EXPECT_FALSE(std::isnan(sigma));
            EXPECT_GT(sigma, 0.0);
        }
    }

    TEST(VolSurfacePipeline, ClampsKRangeToWhatTheNarrowestSliceObserved)
    {
        // A short-dated slice quoted only close to the money (narrow strike
        // range) alongside a long-dated slice with the usual wide range.
        // Requesting a wide k range must not force the short slice's SVI
        // wing to extrapolate past what it actually observed -- found to
        // matter against a real AAPL chain, where a ~6-day slice implied a
        // ~600% vol at the edge of a fixed +/-0.6 log-moneyness request.
        const Real spot = 100.0;
        std::vector<RawOptionQuote> raw;
        for (Real strike = 92.5; strike <= 107.5; strike += 2.5)
        {
            const Real k = std::log(strike / spot);
            const Real iv = svi_implied_vol(k, 0.02, smile(0.005));
            const Real price = bs_call_price(spot, strike, 0.02, 0.0, iv);

            RawOptionQuote q;
            q.strike = strike;
            q.ttm = 0.02;
            q.is_call = true;
            q.bid = price - 0.001;
            q.ask = price + 0.001;
            q.last = price;
            q.volume = 500;
            q.open_interest = 1000;
            q.implied_vol = iv;
            q.has_iv = true;
            raw.push_back(q);
        }
        const std::vector<RawOptionQuote> raw_1y = synthetic_slice(spot, 1.00, smile(0.10));
        raw.insert(raw.end(), raw_1y.begin(), raw_1y.end());

        const auto result = calibrate_vol_surface(raw, spot, 0.0, 0.0, -0.6, 0.6, 30, 15);

        const Real narrow_k_min = std::log(92.5 / 100.0);
        const Real narrow_k_max = std::log(107.5 / 100.0);
        EXPECT_GE(result.k_min, narrow_k_min - 1e-9);
        EXPECT_LE(result.k_max, narrow_k_max + 1e-9);
        EXPECT_LT(result.k_max - result.k_min, 0.6); // much tighter than the requested [-0.6, 0.6]

        for (const Real sigma_loc : result.sigma_loc)
            EXPECT_LT(sigma_loc, 3.0); // no wing blow-up from extrapolating past the short slice's data
    }

    // A few-day slice quoted near the money, next to two wide ones: it no
    // longer narrows the grid, and stays out of the surface.
    TEST(VolSurfacePipeline, ShortSlicesNeitherNarrowTheRangeNorEnterTheSurface)
    {
        const Real spot = 100.0;
        std::vector<RawOptionQuote> raw;
        for (const RawOptionQuote &q : synthetic_slice(spot, 0.02, smile(0.005)))
            if (q.strike >= 92.5 && q.strike <= 107.5)
                raw.push_back(q);
        for (const Real ttm : {0.25, 1.0})
        {
            const std::vector<RawOptionQuote> slice = synthetic_slice(spot, ttm, smile(ttm == 1.0 ? 0.10 : 0.02));
            raw.insert(raw.end(), slice.begin(), slice.end());
        }

        const auto result = calibrate_vol_surface(raw, spot, 0.0, 0.0, -0.6, 0.6, 30, 15);

        ASSERT_EQ(result.slices.size(), 3u);
        EXPECT_FALSE(result.slices[0].in_surface);
        EXPECT_TRUE(result.slices[1].in_surface);
        EXPECT_TRUE(result.slices[2].in_surface);
        EXPECT_LT(result.k_min, std::log(90.0 / spot)); // wider than the short slice's 92.5..107.5
        EXPECT_GT(result.k_max, std::log(110.0 / spot));
        // Columns on (0, 1]: 15 split 5 / 10 between [0, 0.25] and [0.25, 1],
        // with a column on each side of the 0.25 slice.
        ASSERT_EQ(result.T_grid.size(), 15u);
        EXPECT_NEAR(result.T_grid.front(), 0.05, 1e-12);
        EXPECT_EQ(result.T_grid[4], 0.25);
        EXPECT_EQ(result.T_grid[5], 0.25 * (1.0 + 1e-9));
        EXPECT_EQ(result.T_grid.back(), 1.0);
        for (const Real sigma_loc : result.sigma_loc)
        {
            EXPECT_GT(sigma_loc, 0.05);
            EXPECT_LT(sigma_loc, 3.0);
        }
    }

    namespace
    {
        SVISliceCalibration slice_at(Real ttm, Real a)
        {
            SVISliceCalibration s;
            s.ttm = ttm;
            s.params = smile(a);
            return s;
        }

        std::vector<Real> ttms_of(const std::vector<SVISliceCalibration> &slices, const std::vector<std::size_t> &idx)
        {
            std::vector<Real> out;
            for (const std::size_t i : idx)
                out.push_back(slices[i].ttm);
            return out;
        }
    } // namespace

    // Two slices closer than min_relative_gap are never consecutive; between
    // equally long chains, the one with more quotes wins.
    TEST(SurfaceSliceSelection, SpacesThePillarsAndBreaksTiesOnQuotes)
    {
        const std::vector<SVISliceCalibration> slices{slice_at(0.02, 0.001), slice_at(0.25, 0.02), slice_at(0.30, 0.025),
                                                      slice_at(0.5, 0.05), slice_at(1.0, 0.10)};
        EXPECT_EQ(ttms_of(slices, select_surface_slices(slices, {10, 10, 50, 10, 10}, -0.4, 0.4)),
                  (std::vector<Real>{0.30, 0.5, 1.0}));
        EXPECT_EQ(ttms_of(slices, select_surface_slices(slices, {10, 50, 10, 10, 10}, -0.4, 0.4)),
                  (std::vector<Real>{0.25, 0.5, 1.0}));
    }

    // A slice above its neighbours' variance (calendar arbitrage against the
    // ones after it) is the one left out: keeping it would end the chain.
    TEST(SurfaceSliceSelection, LeavesOutTheSliceThatContradictsTheOthers)
    {
        const std::vector<SVISliceCalibration> slices{slice_at(0.25, 0.02), slice_at(0.5, 0.20), slice_at(0.75, 0.07),
                                                      slice_at(1.0, 0.10), slice_at(1.5, 0.14)};
        const std::vector<std::size_t> kept = select_surface_slices(slices, {10, 500, 10, 10, 10}, -0.4, 0.4);
        EXPECT_EQ(ttms_of(slices, kept), (std::vector<Real>{0.25, 0.75, 1.0, 1.5}));
        for (std::size_t i = 0; i + 1 < kept.size(); ++i)
            EXPECT_TRUE(svi_slices_are_calendar_arbitrage_free(slices[kept[i]], slices[kept[i + 1]], -0.4, 0.4));
    }

    // Fewer than two slices reach min_ttm: every slice is a candidate, as
    // before the selection existed.
    TEST(SurfaceSliceSelection, FallsBackToEverySliceWhenTooFewAreLongEnough)
    {
        const std::vector<SVISliceCalibration> slices{slice_at(0.01, 0.001), slice_at(0.02, 0.002), slice_at(1.0, 0.1)};
        EXPECT_EQ(select_surface_slices(slices, {10, 10, 10}, -0.4, 0.4), (std::vector<std::size_t>{0, 1, 2}));
    }

    TEST(VolSurfacePipeline, RecoversAFlatVolatilityGridEndToEnd)
    {
        constexpr Real sigma = 0.22;
        std::vector<RawOptionQuote> raw = synthetic_slice(100.0, 0.25, SVIParams{sigma * sigma * 0.25, 0.0, 0.0, 0.0, 0.1});
        const std::vector<RawOptionQuote> raw_1y =
            synthetic_slice(100.0, 1.00, SVIParams{sigma * sigma * 1.00, 0.0, 0.0, 0.0, 0.1});
        raw.insert(raw.end(), raw_1y.begin(), raw_1y.end());

        const auto result = calibrate_vol_surface(raw, 100.0, 0.0, 0.0, -0.4, 0.4, 25, 12);

        for (const Real sigma_loc : result.sigma_loc)
            EXPECT_NEAR(sigma_loc, sigma, 5e-3);
    }

} // namespace quantModeling
