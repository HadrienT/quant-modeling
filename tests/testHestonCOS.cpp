#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/models/equity/sabr.hpp" // black76_call_price, for the Black-Scholes reduction test

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kForward = 100.0;
        constexpr Real kTtm = 0.75;
        constexpr Real kDiscount = 0.96;
    } // namespace

    // ── Degenerate case: vanishing vol-of-vol reduces to Black-Scholes ──────
    // rho = 0 is deliberate: with v0 = theta and xi -> 0, the limiting
    // process no longer depends on rho (there is no variance-of-variance
    // left to correlate with), and by direct series expansion of D and G in
    // xi, term_v0 + term_theta -> -0.5*theta*ttm*u^2 - i*0.5*theta*ttm*u,
    // exactly the forward-measure GBM log-return characteristic function,
    // when rho = 0.
    //
    // xi = 1e-5 is deliberately extreme: an earlier, more direct
    // formulation of heston_log_return_characteristic_function computed
    // (beta - D) as an explicit subtraction of two O(1) complex numbers
    // whose true difference is O(xi^2), then divided by xi^2 -- a
    // catastrophic-cancellation amplifier that produced prices off by 5+
    // orders of magnitude at this xi (caught by this exact test before the
    // fix). The current implementation instead derives (beta - D) from the
    // identity beta^2 - D^2 = -xi^2*(u^2+iu), a division of two well-scaled
    // quantities with no cancellation -- this test's tight tolerance at an
    // extreme xi is what pins that fix in place.

    TEST(HestonCOS, ReducesToBlackScholesWhenVolOfVolVanishes)
    {
        const Real sigma = 0.25;
        const HestonParams p{/*v0=*/sigma * sigma, /*kappa=*/1.5, /*theta=*/sigma * sigma, /*xi=*/1e-5, /*rho=*/0.0};

        for (const Real strike : {70.0, 90.0, 100.0, 110.0, 135.0})
        {
            const Real heston_price = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/true);
            const Real bs_price = black76_call_price(kForward, strike, kTtm, sigma, kDiscount);
            EXPECT_NEAR(heston_price, bs_price, 1e-4) << "strike=" << strike;
        }
    }

    // ── Put-call parity ──────────────────────────────────────────────────────

    TEST(HestonCOS, SatisfiesPutCallParity)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        for (const Real strike : {80.0, 100.0, 120.0})
        {
            const Real call = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/true);
            const Real put = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/false);
            EXPECT_NEAR(call - put, kDiscount * (kForward - strike), 1e-8);
        }
    }

    // ── No-arbitrage bounds ───────────────────────────────────────────────────

    TEST(HestonCOS, CallPriceRespectsNoArbitrageBounds)
    {
        const HestonParams p{0.05, 1.2, 0.05, 0.5, -0.7};
        for (const Real strike : {60.0, 100.0, 140.0})
        {
            const Real call = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/true);
            const Real intrinsic = kDiscount * std::max(kForward - strike, 0.0);
            EXPECT_GE(call, intrinsic - 1e-8);
            EXPECT_LE(call, kDiscount * kForward + 1e-8);
        }
    }

    TEST(HestonCOS, PutPriceRespectsNoArbitrageBounds)
    {
        const HestonParams p{0.05, 1.2, 0.05, 0.5, -0.7};
        for (const Real strike : {60.0, 100.0, 140.0})
        {
            const Real put = heston_cos_price(kForward, strike, kTtm, kDiscount, p, /*is_call=*/false);
            const Real intrinsic = kDiscount * std::max(strike - kForward, 0.0);
            EXPECT_GE(put, intrinsic - 1e-8);
            EXPECT_LE(put, kDiscount * strike + 1e-8);
        }
    }

    // ── Monotonicity ──────────────────────────────────────────────────────────

    TEST(HestonCOS, CallPriceIsDecreasingInStrike)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        Real previous = heston_cos_price(kForward, 60.0, kTtm, kDiscount, p, true);
        for (const Real strike : {70.0, 85.0, 100.0, 115.0, 130.0, 150.0})
        {
            const Real price = heston_cos_price(kForward, strike, kTtm, kDiscount, p, true);
            EXPECT_LT(price, previous);
            previous = price;
        }
    }

    // ── Convergence in the number of cosine terms ────────────────────────────

    TEST(HestonCOS, PriceStabilizesAsTermCountIncreases)
    {
        const HestonParams p{0.04, 1.5, 0.045, 0.4, -0.6};
        HestonCOSSettings coarse;
        coarse.n_terms = 32;
        HestonCOSSettings fine;
        fine.n_terms = 256;

        for (const Real strike : {80.0, 100.0, 120.0})
        {
            const Real coarse_price = heston_cos_price(kForward, strike, kTtm, kDiscount, p, true, coarse);
            const Real fine_price = heston_cos_price(kForward, strike, kTtm, kDiscount, p, true, fine);
            EXPECT_NEAR(coarse_price, fine_price, 1e-2) << "strike=" << strike;
        }
    }

    TEST(HestonCOS, FellerViolatingParamsStillProduceAFiniteSaneCallPrice)
    {
        // Feller's condition being violated (variance can touch zero) is a
        // modeling caveat, not a reason the characteristic function or the
        // pricing formula should misbehave -- see feller_condition_satisfied's
        // doc comment.
        const HestonParams p{0.04, 0.5, 0.04, 0.9, -0.7}; // 2*0.5*0.04=0.04 < 0.81
        ASSERT_FALSE(feller_condition_satisfied(p));

        const Real call = heston_cos_price(kForward, 100.0, kTtm, kDiscount, p, true);
        EXPECT_TRUE(std::isfinite(call));
        EXPECT_GT(call, 0.0);
        EXPECT_LT(call, kDiscount * kForward);
    }

} // namespace quantModeling
