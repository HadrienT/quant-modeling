#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/engines/mc/heston_qe.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kForward = 100.0;
        constexpr Real kTtm = 0.5;
        constexpr Real kDiscount = 0.97;

        HestonQESettings fast_settings()
        {
            HestonQESettings s;
            s.n_steps = 50;
            s.n_paths = 200000;
            s.seed = 42;
            return s;
        }
    } // namespace

    // ── Cross-validation against the (independently verified) COS pricer ────

    TEST(HestonQE, PriceAgreesWithCOSWithinMonteCarloError)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        const auto settings = fast_settings();

        for (const Real strike : {80.0, 100.0, 120.0})
        {
            const HestonQEResult mc = heston_qe_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/true, settings);
            const Real analytic = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/true);

            ASSERT_GT(mc.std_error, 0.0);
            EXPECT_NEAR(mc.price, analytic, 4.0 * mc.std_error) << "strike=" << strike;
        }
    }

    // ── Martingale correction: a pathwise identity, not just a statistical one ──
    // call_payoff - put_payoff = F_T - K on every single path (by definition of
    // the two payoffs), so with the same RNG seed driving both runs, the
    // Monte-Carlo estimate of (call - put) is exactly discount*(mean(F_T) - K)
    // -- no additional sampling noise beyond whatever noise mean(F_T) already
    // has. This isolates and directly tests the QE-M martingale correction
    // (mean(F_T) should be close to the forward) far more tightly than
    // comparing full option prices against COS would.

    TEST(HestonQE, PutCallParityHoldsPathwiseUnderTheSameSeed)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        const auto settings = fast_settings();
        const Real strike = 100.0;

        const HestonQEResult call = heston_qe_price(kForward, strike, kTtm, kDiscount, p, true, settings);
        const HestonQEResult put = heston_qe_price(kForward, strike, kTtm, kDiscount, p, false, settings);

        EXPECT_NEAR(call.price - put.price, kDiscount * (kForward - strike), 4.0 * call.std_error);
    }

    // ── Reproducibility ───────────────────────────────────────────────────────

    TEST(HestonQE, IsReproducibleGivenTheSameSeed)
    {
        const HestonParams p{0.05, 1.2, 0.05, 0.5, -0.5};
        const auto settings = fast_settings();

        const HestonQEResult first = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, settings);
        const HestonQEResult second = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, settings);

        EXPECT_DOUBLE_EQ(first.price, second.price);
        EXPECT_DOUBLE_EQ(first.std_error, second.std_error);
    }

    TEST(HestonQE, DifferentSeedsGiveDifferentButCloseEstimates)
    {
        const HestonParams p{0.05, 1.2, 0.05, 0.5, -0.5};
        auto settings_a = fast_settings();
        auto settings_b = fast_settings();
        settings_b.seed = 43;

        const HestonQEResult a = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, settings_a);
        const HestonQEResult b = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, settings_b);

        EXPECT_NE(a.price, b.price);
        EXPECT_NEAR(a.price, b.price, 6.0 * (a.std_error + b.std_error));
    }

    // ── Degenerate inputs guard cleanly ────────────────────────────────────────

    TEST(HestonQE, ZeroPathsOrZeroStepsReturnsZeroWithoutCrashing)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};

        HestonQESettings zero_paths = fast_settings();
        zero_paths.n_paths = 0;
        const HestonQEResult r1 = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, zero_paths);
        EXPECT_DOUBLE_EQ(r1.price, 0.0);
        EXPECT_DOUBLE_EQ(r1.std_error, 0.0);

        HestonQESettings zero_steps = fast_settings();
        zero_steps.n_steps = 0;
        const HestonQEResult r2 = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, zero_steps);
        EXPECT_DOUBLE_EQ(r2.price, 0.0);
        EXPECT_DOUBLE_EQ(r2.std_error, 0.0);
    }

    TEST(HestonQE, ZeroTtmReturnsZeroWithoutCrashing)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        const HestonQEResult r = heston_qe_price(kForward, 100.0, 0.0, kDiscount, p, true, fast_settings());
        EXPECT_DOUBLE_EQ(r.price, 0.0);
    }

    // ── Positive-correlation regularity fallback (see header doc comment) ────

    TEST(HestonQE, PositiveRhoStillProducesAFiniteSanePrice)
    {
        // The martingale-correction regularity condition can in principle
        // fail for rho > 0 with a large step; this must fall back cleanly
        // rather than propagate a NaN/inf, per the header doc comment.
        const HestonParams p{0.04, 1.5, 0.045, 0.4, 0.6};
        HestonQESettings settings = fast_settings();
        settings.n_steps = 4; // deliberately coarse

        const HestonQEResult r = heston_qe_price(kForward, 100.0, kTtm, kDiscount, p, true, settings);
        EXPECT_TRUE(std::isfinite(r.price));
        EXPECT_GE(r.price, 0.0);
        EXPECT_LE(r.price, kDiscount * kForward);
    }

} // namespace quantModeling
