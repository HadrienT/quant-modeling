#include <gtest/gtest.h>

#include "quantModeling/market/raw_vol_surface.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        RawOptionQuote make_quote(Real strike, Real ttm, bool is_call, Real bid, Real ask, Real last,
                                  std::int64_t volume, std::int64_t open_interest, Real iv)
        {
            RawOptionQuote q;
            q.strike = strike;
            q.ttm = ttm;
            q.is_call = is_call;
            q.bid = bid;
            q.ask = ask;
            q.last = last;
            q.volume = volume;
            q.open_interest = open_interest;
            q.implied_vol = iv;
            q.has_iv = true;
            return q;
        }

        /// A liquid, tightly-quoted call with no arbitrage concerns by itself.
        RawOptionQuote liquid_call(Real strike, Real ttm, Real mid_price, Real iv)
        {
            return make_quote(strike, ttm, true, mid_price - 0.01, mid_price + 0.01, mid_price, 500, 1000, iv);
        }

    } // namespace

    // ── Forward / log-moneyness ─────────────────────────────────────────────

    TEST(RawVolSurface, ForwardMatchesHandComputedReference)
    {
        RawVolSurface surface({}, /*spot=*/100.0, /*rate=*/0.03, /*dividend=*/0.01);
        const Real T = 0.5;
        const Real expected = 100.0 * std::exp((0.03 - 0.01) * T);
        EXPECT_NEAR(surface.forward(T), expected, 1e-12);
    }

    TEST(RawVolSurface, LogMoneynessIsZeroAtTheForward)
    {
        RawVolSurface surface({}, 100.0, 0.03, 0.01);
        const Real T = 0.5;
        const Real F = surface.forward(T);
        EXPECT_NEAR(surface.log_moneyness(F, T), 0.0, 1e-12);
        EXPECT_GT(surface.log_moneyness(F * 1.1, T), 0.0);
        EXPECT_LT(surface.log_moneyness(F * 0.9, T), 0.0);
    }

    // ── Liquidity filter ─────────────────────────────────────────────────────

    TEST(RawVolSurface, LiquidityFilterDropsLowOpenInterestWhenOICoverageIsGood)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(100.0, 0.25, 5.0, 0.20),                          // OI=1000, kept
            make_quote(105.0, 0.25, true, 2.99, 3.01, 3.0, 500, 2, 0.20), // OI=2, dropped
        };
        // Pad with liquid quotes so OI coverage is judged "available" (>5%).
        for (int i = 0; i < 10; ++i)
            raw.push_back(liquid_call(90.0 + i, 0.25, 4.0, 0.20));

        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        for (const auto &q : surface.quotes())
            EXPECT_GE(q.open_interest, 10);
        EXPECT_EQ(surface.stats().raw_count, raw.size());
        EXPECT_LT(surface.stats().after_liquidity, raw.size());
    }

    TEST(RawVolSurface, LiquidityFilterRejectsAQuoteWithNoImpliedVol)
    {
        RawOptionQuote q = liquid_call(100.0, 0.25, 5.0, 0.20);
        q.has_iv = false;
        RawVolSurface surface({q}, 100.0, 0.0, 0.0);
        EXPECT_EQ(surface.stats().after_liquidity, 0u);
    }

    TEST(RawVolSurface, LiquidityFilterRejectsAWideSpread)
    {
        RawOptionQuote wide = make_quote(100.0, 0.25, true, 1.0, 9.0, 5.0, 500, 1000, 0.20); // spread/mid = 8/5 = 1.6
        RawVolSurface surface({wide}, 100.0, 0.0, 0.0);
        EXPECT_EQ(surface.stats().after_liquidity, 0u);
    }

    TEST(RawVolSurface, LiquidityFilterRejectsAnImplausibleImpliedVolEvenWithHighOpenInterest)
    {
        // Found against a real AAPL chain, not hypothesised: yfinance can
        // report an implausible IV (stale last-trade price) on a contract
        // that otherwise looks perfectly liquid. High OI must not save it.
        RawOptionQuote blown_up = liquid_call(100.0, 0.02, 5.0, 4.50);    // 450% IV
        RawOptionQuote collapsed = liquid_call(105.0, 0.02, 5.0, 0.0001); // ~0% IV
        RawVolSurface surface({blown_up, collapsed}, 100.0, 0.0, 0.0);
        EXPECT_EQ(surface.stats().after_liquidity, 0u);
    }

    TEST(RawVolSurface, LiquidityFilterFallsBackToVolumeWhenOpenInterestIsMostlyAbsent)
    {
        std::vector<RawOptionQuote> raw;
        for (int i = 0; i < 20; ++i)
        {
            RawOptionQuote q = liquid_call(90.0 + i, 0.25, 4.0, 0.20);
            q.open_interest = 0;           // every quote reports no OI: falls back to volume
            q.volume = (i == 0) ? 0 : 500; // one illiquid-by-volume quote
            raw.push_back(q);
        }
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        EXPECT_EQ(surface.stats().after_liquidity, raw.size() - 1);
        for (const auto &q : surface.quotes())
            EXPECT_GT(q.volume, 0);
    }

    // ── Moneyness filter ────────────────────────────────────────────────────

    TEST(RawVolSurface, MoneynessFilterKeepsOnlyStrikesWithinBounds)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(50.0, 0.25, 50.0, 0.30), // moneyness 0.5, out
            liquid_call(100.0, 0.25, 5.0, 0.20), // moneyness 1.0, in
            liquid_call(200.0, 0.25, 1.0, 0.30), // moneyness 2.0, out
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        ASSERT_EQ(surface.quotes().size(), 1u);
        EXPECT_DOUBLE_EQ(surface.quotes().front().strike, 100.0);
    }

    // ── Calendar arbitrage ──────────────────────────────────────────────────

    TEST(RawVolSurface, CalendarArbitrageDropsTheMaturityWhoseTotalVarianceDecreases)
    {
        // Same strike, two maturities: w(T1) = 0.30^2*0.25 = 0.0225,
        // w(T2) = 0.15^2*0.50 = 0.01125 < w(T1) -- a calendar-arbitrage
        // violation, so the longer-dated quote must be dropped.
        std::vector<RawOptionQuote> raw = {
            liquid_call(100.0, 0.25, 5.0, 0.30),
            liquid_call(100.0, 0.50, 6.0, 0.15),
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);

        ASSERT_EQ(surface.quotes().size(), 1u);
        EXPECT_NEAR(surface.quotes().front().ttm, 0.25, 1e-12);
        EXPECT_LT(surface.stats().after_calendar_arbitrage, surface.stats().after_moneyness);
    }

    TEST(RawVolSurface, CalendarArbitrageKeepsATrulyIncreasingTotalVarianceSeries)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(100.0, 0.25, 5.0, 0.20),
            liquid_call(100.0, 0.50, 7.0, 0.22),
            liquid_call(100.0, 1.00, 10.0, 0.25),
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        EXPECT_EQ(surface.quotes().size(), raw.size());
        EXPECT_EQ(surface.stats().after_calendar_arbitrage, surface.stats().after_moneyness);
    }

    TEST(RawVolSurface, SurvivingQuotesHaveNonDecreasingTotalVarianceInMaturity)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(100.0, 0.10, 3.0, 0.35),
            liquid_call(100.0, 0.25, 5.0, 0.15), // violates vs. the 0.10y quote
            liquid_call(100.0, 0.50, 6.0, 0.25),
            liquid_call(100.0, 1.00, 9.0, 0.10), // violates vs. everything before it
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);

        std::vector<RawOptionQuote> survivors = surface.quotes();
        std::sort(survivors.begin(), survivors.end(),
                  [](const RawOptionQuote &a, const RawOptionQuote &b)
                  { return a.ttm < b.ttm; });

        Real prev_w = -1.0;
        for (const auto &q : survivors)
        {
            const Real w = q.implied_vol * q.implied_vol * q.ttm;
            EXPECT_GE(w, prev_w);
            prev_w = w;
        }
    }

    // ── Butterfly arbitrage ──────────────────────────────────────────────────

    TEST(RawVolSurface, ButterflyArbitrageDropsTheMispricedMiddleStrike)
    {
        // C(K1) - 2*C(K2) + C(K3) = 5 - 20 + 5 = -10 < 0: K2 is mispriced high
        // relative to its neighbours and must be dropped.
        std::vector<RawOptionQuote> raw = {
            liquid_call(90.0, 0.25, 5.0, 0.30),
            liquid_call(100.0, 0.25, 10.0, 0.30),
            liquid_call(110.0, 0.25, 5.0, 0.30),
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);

        ASSERT_EQ(surface.quotes().size(), 2u);
        for (const auto &q : surface.quotes())
            EXPECT_NE(q.strike, 100.0);
        EXPECT_LT(surface.stats().after_butterfly_arbitrage, surface.stats().after_calendar_arbitrage);
    }

    TEST(RawVolSurface, ButterflyArbitrageLeavesPutsUntouched)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(90.0, 0.25, 5.0, 0.30),
            liquid_call(100.0, 0.25, 10.0, 0.30), // would violate convexity...
            liquid_call(110.0, 0.25, 5.0, 0.30),
            make_quote(100.0, 0.25, /*is_call=*/false, 4.99, 5.01, 5.0, 500, 1000, 0.30), // ...but this is a put
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);

        bool put_survived = false;
        for (const auto &q : surface.quotes())
            if (!q.is_call)
                put_survived = true;
        EXPECT_TRUE(put_survived);
    }

    TEST(RawVolSurface, SurvivingCallsAreConvexInStrikePerMaturity)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(80.0, 0.25, 21.0, 0.30),
            liquid_call(90.0, 0.25, 12.0, 0.30),
            liquid_call(100.0, 0.25, 6.0, 0.30),
            liquid_call(110.0, 0.25, 8.0, 0.30), // deliberately too high: breaks convexity locally
            liquid_call(120.0, 0.25, 1.0, 0.30),
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);

        std::vector<RawOptionQuote> calls = surface.quotes();
        std::sort(calls.begin(), calls.end(),
                  [](const RawOptionQuote &a, const RawOptionQuote &b)
                  { return a.strike < b.strike; });

        for (std::size_t i = 1; i + 1 < calls.size(); ++i)
        {
            const Real c0 = calls[i - 1].mid();
            const Real c1 = calls[i].mid();
            const Real c2 = calls[i + 1].mid();
            EXPECT_GE(c0 - 2.0 * c1 + c2, -1e-9);
        }
    }

    // ── End to end ────────────────────────────────────────────────────────────

    TEST(RawVolSurface, StatsCountsDecreaseMonotonicallyThroughTheCleaningPipeline)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(50.0, 0.25, 50.0, 0.30),                           // dropped: moneyness
            make_quote(100.0, 0.25, true, 1.0, 9.0, 5.0, 500, 1000, 0.30), // dropped: wide spread
            liquid_call(100.0, 0.25, 5.0, 0.30),
            liquid_call(100.0, 0.50, 6.0, 0.15), // dropped: calendar arb
            liquid_call(90.0, 1.0, 12.0, 0.30),
            liquid_call(100.0, 1.0, 8.0, 0.30), // mispriced middle strike
            liquid_call(110.0, 1.0, 5.0, 0.30), // -> dropped: butterfly arb
        };
        RawVolSurface surface(raw, 100.0, 0.0, 0.0);
        const auto &s = surface.stats();

        EXPECT_EQ(s.raw_count, raw.size());
        EXPECT_GE(s.after_liquidity, s.after_moneyness);
        EXPECT_GE(s.after_moneyness, s.after_calendar_arbitrage);
        EXPECT_GE(s.after_calendar_arbitrage, s.after_butterfly_arbitrage);
        EXPECT_EQ(s.final_count, s.after_butterfly_arbitrage);
        EXPECT_EQ(s.final_count, surface.quotes().size());
        EXPECT_LT(s.final_count, s.raw_count);
    }

    TEST(RawVolSurface, IsDeterministic)
    {
        std::vector<RawOptionQuote> raw = {
            liquid_call(90.0, 0.25, 12.0, 0.28),
            liquid_call(100.0, 0.25, 6.0, 0.25),
            liquid_call(110.0, 0.25, 3.0, 0.30),
        };
        RawVolSurface s1(raw, 100.0, 0.02, 0.0);
        RawVolSurface s2(raw, 100.0, 0.02, 0.0);
        ASSERT_EQ(s1.quotes().size(), s2.quotes().size());
        for (std::size_t i = 0; i < s1.quotes().size(); ++i)
            EXPECT_DOUBLE_EQ(s1.quotes()[i].strike, s2.quotes()[i].strike);
    }

} // namespace quantModeling
