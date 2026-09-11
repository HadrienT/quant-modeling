#include <gtest/gtest.h>

#include "quantModeling/market/raw_vol_surface.hpp"
#include "quantModeling/market/svi_calibration.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        SVIParams nice_params()
        {
            SVIParams p;
            p.a = 0.04;
            p.b = 0.10;
            p.rho = -0.40;
            p.m = 0.0;
            p.sigma = 0.30;
            return p;
        }

        Real bs_call_price(Real S, Real K, Real T, Real r, Real sigma)
        {
            const Real vol_sqrt_t = sigma * std::sqrt(T);
            const Real d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / vol_sqrt_t;
            const Real d2 = d1 - vol_sqrt_t;
            return S * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d2);
        }

        /// A synthetic, arbitrage-free option chain generated from a known
        /// SVI slice: every quote's implied vol and price agree exactly with
        /// `true_params`, so nothing should be dropped by RawVolSurface's
        /// cleaning stage and SVI calibration should recover the slice.
        std::vector<RawOptionQuote> synthetic_chain(Real spot, Real ttm, const SVIParams &true_params)
        {
            std::vector<RawOptionQuote> raw;
            for (Real strike = 75.0; strike <= 130.0; strike += 2.5)
            {
                const Real k = std::log(strike / spot); // r = q = 0, so F = spot
                const Real iv = svi_implied_vol(k, ttm, true_params);
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

    } // namespace

    // ── svi_quotes_from_raw_surface ─────────────────────────────────────────

    TEST(SVICalibration, ExtractsQuotesOnlyAtTheRequestedMaturity)
    {
        // Two full, independently uniform strike grids at two maturities --
        // deliberately not a single grid with one point plucked out, which
        // would leave a gap that the (spacing-sensitive, by design) discrete
        // butterfly check legitimately flags. The 1y slice uses a distinctly
        // larger `a` so its total variance is unambiguously larger at every
        // strike (not merely equal up to floating-point round-trip), keeping
        // the calendar-arbitrage stage from touching either grid.
        SVIParams params_1y = nice_params();
        params_1y.a += 0.05;

        std::vector<RawOptionQuote> raw = synthetic_chain(100.0, 0.5, nice_params());
        const std::vector<RawOptionQuote> raw_1y = synthetic_chain(100.0, 1.0, params_1y);
        const std::size_t n_at_05 = raw.size();
        const std::size_t n_at_10 = raw_1y.size();
        raw.insert(raw.end(), raw_1y.begin(), raw_1y.end());

        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        const auto quotes_05 = svi_quotes_from_raw_surface(surface, 0.5);
        const auto quotes_10 = svi_quotes_from_raw_surface(surface, 1.0);

        EXPECT_EQ(quotes_05.size(), n_at_05);
        EXPECT_EQ(quotes_10.size(), n_at_10);
    }

    TEST(SVICalibration, ExtractedLogMoneynessMatchesTheSurfaceCoordinate)
    {
        std::vector<RawOptionQuote> raw = synthetic_chain(100.0, 0.5, nice_params());
        RawVolSurface surface(raw, 100.0, 0.02, 0.0);
        const auto quotes = svi_quotes_from_raw_surface(surface, 0.5);

        std::vector<RawOptionQuote> raw_at_05;
        for (const auto &q : surface.quotes())
            if (q.has_iv && q.ttm == 0.5)
                raw_at_05.push_back(q);

        ASSERT_FALSE(quotes.empty());
        ASSERT_EQ(quotes.size(), raw_at_05.size());
        for (std::size_t i = 0; i < quotes.size(); ++i)
        {
            EXPECT_NEAR(quotes[i].log_moneyness, surface.log_moneyness(raw_at_05[i].strike, 0.5), 1e-9);
            EXPECT_DOUBLE_EQ(quotes[i].market_iv, raw_at_05[i].implied_vol);
            EXPECT_GT(quotes[i].weight, 0.0);
        }
    }

    // ── calibrate_svi_slice ──────────────────────────────────────────────────

    TEST(SVICalibration, EmptyQuotesDoNotCrashAndAreNotArbitrageFree)
    {
        const auto result = calibrate_svi_slice({}, 0.5);
        EXPECT_FALSE(result.butterfly_arbitrage_free);
    }

    TEST(SVICalibration, RecoversTheSmileFunctionallyFromNoiselessSyntheticQuotes)
    {
        const Real spot = 100.0, ttm = 0.5;
        const SVIParams truth = nice_params();

        std::vector<RawOptionQuote> raw = synthetic_chain(spot, ttm, truth);
        RawVolSurface surface(raw, spot, 0.0, 0.0);
        const auto quotes = svi_quotes_from_raw_surface(surface, ttm);
        ASSERT_GE(quotes.size(), 10u);

        const auto result = calibrate_svi_slice(quotes, ttm);

        EXPECT_TRUE(result.report.converged);
        // SVI's parameters are not always individually identifiable from a
        // bounded strike range (several (a,b,rho,m,sigma) can trace nearly the
        // same curve), so the property that must hold is the *fit*, not
        // parameter equality: the calibrated curve must reproduce the true
        // smile at every quoted point.
        for (const auto &sq : quotes)
        {
            const Real fitted_iv = svi_implied_vol(sq.log_moneyness, ttm, result.params);
            EXPECT_NEAR(fitted_iv, sq.market_iv, 2e-3);
        }
        EXPECT_LT(result.report.rmse, 2e-3);
        EXPECT_TRUE(result.butterfly_arbitrage_free);
    }

    // ── Calendar-arbitrage check between calibrated slices ──────────────────

    TEST(SVICalibration, DetectsCalendarArbitrageBetweenTwoCalibratedSlices)
    {
        SVISliceCalibration shorter;
        shorter.ttm = 0.25;
        shorter.params = nice_params();
        shorter.params.a = 0.08; // deliberately large total variance for the short maturity

        SVISliceCalibration longer;
        longer.ttm = 0.50;
        longer.params = nice_params();
        longer.params.a = 0.04; // and small for the long one: total variance must decrease -- arbitrage

        EXPECT_FALSE(svi_slices_are_calendar_arbitrage_free(shorter, longer, -1.0, 1.0));
    }

    TEST(SVICalibration, AcceptsATrulyIncreasingCalendarPair)
    {
        SVISliceCalibration shorter;
        shorter.ttm = 0.25;
        shorter.params = nice_params();
        shorter.params.a = 0.02;

        SVISliceCalibration longer;
        longer.ttm = 0.50;
        longer.params = nice_params();
        longer.params.a = 0.06; // strictly more total variance everywhere on the grid

        EXPECT_TRUE(svi_slices_are_calendar_arbitrage_free(shorter, longer, -1.0, 1.0));
    }

    TEST(SVICalibration, RejectsSlicesPassedOutOfMaturityOrder)
    {
        SVISliceCalibration a;
        a.ttm = 0.5;
        a.params = nice_params();
        SVISliceCalibration b;
        b.ttm = 0.25;
        b.params = nice_params();

        EXPECT_FALSE(svi_slices_are_calendar_arbitrage_free(a, b, -1.0, 1.0));
    }

} // namespace quantModeling
