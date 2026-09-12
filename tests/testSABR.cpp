#include <gtest/gtest.h>

#include "quantModeling/models/equity/sabr.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kForward = 100.0;
        constexpr Real kTtm = 1.0;
    } // namespace

    // ── Degenerate case: beta=1, nu=0 is pure lognormal with constant vol ──
    // alpha -- a clean, exactly-known answer for every strike, not just one
    // worked example.

    TEST(SABR, ReducesToFlatLognormalVolWhenBetaIsOneAndNuIsZero)
    {
        const SABRParams p{/*alpha=*/0.25, /*beta=*/1.0, /*rho=*/0.0, /*nu=*/0.0};
        for (const Real K : {60.0, 80.0, 95.0, 100.0, 105.0, 120.0, 150.0})
            EXPECT_NEAR(sabr_implied_vol(kForward, K, kTtm, p), 0.25, 1e-10);
    }

    TEST(SABR, ReducesToFlatVolAtAnyMaturityInTheDegenerateCase)
    {
        const SABRParams p{0.30, 1.0, 0.0, 0.0};
        for (const Real T : {0.05, 0.5, 1.0, 5.0})
            EXPECT_NEAR(sabr_implied_vol(kForward, 90.0, T, p), 0.30, 1e-10);
    }

    // ── ATM branch matches the general branch's limit ───────────────────────

    TEST(SABR, AtmFormulaAgreesWithTheGeneralFormulaApproachingAtm)
    {
        const SABRParams p{0.25, 0.6, -0.3, 0.5};
        const Real atm = sabr_implied_vol(kForward, kForward, kTtm, p);
        const Real near_atm = sabr_implied_vol(kForward, kForward * 1.00001, kTtm, p);
        EXPECT_NEAR(atm, near_atm, 1e-5);
    }

    // ── Smile shape sanity: negative rho tilts the smile (equity-style skew) ──

    TEST(SABR, NegativeRhoProducesHigherVolAtLowStrikesThanHighStrikes)
    {
        const SABRParams p{0.25, 0.7, -0.5, 0.6};
        const Real low = sabr_implied_vol(kForward, 80.0, kTtm, p);
        const Real high = sabr_implied_vol(kForward, 120.0, kTtm, p);
        EXPECT_GT(low, high);
    }

    // ── Black-76 ─────────────────────────────────────────────────────────────

    TEST(SABR, Black76PriceMatchesIntrinsicValueAtZeroVol)
    {
        EXPECT_NEAR(black76_call_price(100.0, 90.0, 1.0, 0.0, 1.0), 10.0, 1e-12);
        EXPECT_NEAR(black76_call_price(100.0, 110.0, 1.0, 0.0, 1.0), 0.0, 1e-12);
    }

    TEST(SABR, Black76ImpliedVolRoundTripsThroughThePriceFormula)
    {
        for (const Real K : {70.0, 85.0, 100.0, 115.0, 130.0})
        {
            for (const Real vol : {0.10, 0.25, 0.50, 0.90})
            {
                const Real df = 0.97;
                const Real price = black76_call_price(kForward, K, kTtm, vol, df);
                const Real recovered = black76_implied_vol(price, kForward, K, kTtm, df);
                ASSERT_FALSE(std::isnan(recovered)) << "K=" << K << " vol=" << vol;
                EXPECT_NEAR(recovered, vol, 1e-6);
            }
        }
    }

    TEST(SABR, Black76ImpliedVolIsNanForAPriceOutsideNoArbitrageBounds)
    {
        const Real df = 0.97;
        const Real intrinsic = df * std::max(kForward - 90.0, 0.0);
        const Real upper_bound = df * kForward;
        EXPECT_TRUE(std::isnan(black76_implied_vol(intrinsic - 1.0, kForward, 90.0, kTtm, df)));
        EXPECT_TRUE(std::isnan(black76_implied_vol(upper_bound + 1.0, kForward, 90.0, kTtm, df)));
    }

    TEST(SABR, Black76VegaMatchesACentralFiniteDifferenceOfPrice)
    {
        const Real df = 0.97, K = 105.0, vol = 0.3;
        const Real h = 1e-6;
        const Real fd = (black76_call_price(kForward, K, kTtm, vol + h, df) -
                         black76_call_price(kForward, K, kTtm, vol - h, df)) /
                        (2.0 * h);
        EXPECT_NEAR(black76_vega(kForward, K, kTtm, vol, df), fd, 1e-6);
    }

} // namespace quantModeling
