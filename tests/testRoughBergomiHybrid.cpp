#include <gtest/gtest.h>

#include "quantModeling/engines/mc/rough_bergomi_hybrid.hpp"
#include "quantModeling/models/equity/sabr.hpp" // black76_call_price, for the Black-Scholes reduction test

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kForward = 100.0;
        constexpr Real kTtm = 0.5;
        constexpr Real kDiscount = 0.97;

        RoughBergomiSettings fast_settings()
        {
            RoughBergomiSettings s;
            s.n_steps = 50;
            s.n_paths = 50000;
            s.seed = 42;
            return s;
        }

        std::function<Real(Real)> flat_curve(Real level)
        {
            return [level](Real)
            { return level; };
        }

    } // namespace

    // ── Degenerate case: vanishing vol-of-vol reduces to Black-Scholes ──────
    // With eta -> 0, V_t -> xi_0(t) deterministically (the exp() term's
    // argument vanishes), so a flat forward-variance curve xi_0(t) = sigma^2
    // reduces the model to plain Black-76 with vol = sigma, regardless of H
    // or rho (there is no stochastic driver left to depend on them).

    TEST(RoughBergomiHybrid, ReducesToBlackScholesWhenEtaVanishes)
    {
        const Real sigma = 0.25;
        RoughBergomiParams p;
        p.H = 0.1;
        p.eta = 1e-4;
        p.rho = -0.6;

        auto settings = fast_settings();
        const auto xi0 = flat_curve(sigma * sigma);

        for (const Real strike : {80.0, 100.0, 120.0})
        {
            const RoughBergomiResult mc = rough_bergomi_price(kForward, strike, kTtm, kDiscount, p, xi0, true, settings);
            const Real bs = black76_call_price(kForward, strike, kTtm, sigma, kDiscount);
            ASSERT_GT(mc.std_error, 0.0);
            EXPECT_NEAR(mc.price, bs, 4.0 * mc.std_error) << "strike=" << strike;
        }
    }

    // ── Put-call parity, pathwise (same rationale as HestonQE's) ────────────

    TEST(RoughBergomiHybrid, PutCallParityHoldsPathwiseUnderTheSameSeed)
    {
        RoughBergomiParams p;
        p.H = 0.1;
        p.eta = 1.9;
        p.rho = -0.7;
        const auto settings = fast_settings();
        const auto xi0 = flat_curve(0.04);
        const Real strike = 100.0;

        const RoughBergomiResult call = rough_bergomi_price(kForward, strike, kTtm, kDiscount, p, xi0, true, settings);
        const RoughBergomiResult put = rough_bergomi_price(kForward, strike, kTtm, kDiscount, p, xi0, false, settings);

        EXPECT_NEAR(call.price - put.price, kDiscount * (kForward - strike), 4.0 * call.std_error);
    }

    // ── No-arbitrage bounds ───────────────────────────────────────────────────

    TEST(RoughBergomiHybrid, CallPriceRespectsNoArbitrageBounds)
    {
        RoughBergomiParams p;
        p.H = 0.07;
        p.eta = 1.9;
        p.rho = -0.9;
        const auto settings = fast_settings();
        const auto xi0 = flat_curve(0.05);

        for (const Real strike : {70.0, 100.0, 130.0})
        {
            const RoughBergomiResult call = rough_bergomi_price(kForward, strike, kTtm, kDiscount, p, xi0, true, settings);
            const Real intrinsic = kDiscount * std::max(kForward - strike, 0.0);
            EXPECT_GE(call.price, intrinsic - 6.0 * call.std_error);
            EXPECT_LE(call.price, kDiscount * kForward + 1e-8);
        }
    }

    // ── Reproducibility ───────────────────────────────────────────────────────

    TEST(RoughBergomiHybrid, IsReproducibleGivenTheSameSeed)
    {
        RoughBergomiParams p;
        p.H = 0.1;
        p.eta = 1.5;
        p.rho = -0.5;
        const auto settings = fast_settings();
        const auto xi0 = flat_curve(0.045);

        const RoughBergomiResult first = rough_bergomi_price(kForward, 100.0, kTtm, kDiscount, p, xi0, true, settings);
        const RoughBergomiResult second = rough_bergomi_price(kForward, 100.0, kTtm, kDiscount, p, xi0, true, settings);

        EXPECT_DOUBLE_EQ(first.price, second.price);
        EXPECT_DOUBLE_EQ(first.std_error, second.std_error);
    }

    // ── Very rough (H close to 0) still produces a finite, sane price ────────
    // H close to 0 (alpha close to -1/2) is the "explosive short-time
    // variance" regime the model exists to capture (McCrickerd & Pakkanen,
    // Figure 1) -- exactly where a naive implementation of the singular
    // kernel is most likely to misbehave numerically.

    TEST(RoughBergomiHybrid, VeryRoughParametersStillProduceAFiniteSanePrice)
    {
        RoughBergomiParams p;
        p.H = 0.02;
        p.eta = 1.9;
        p.rho = -0.9;
        const auto settings = fast_settings();
        const auto xi0 = flat_curve(0.04);

        const RoughBergomiResult call = rough_bergomi_price(kForward, 100.0, kTtm, kDiscount, p, xi0, true, settings);
        EXPECT_TRUE(std::isfinite(call.price));
        EXPECT_GT(call.price, 0.0);
        EXPECT_LT(call.price, kDiscount * kForward);
    }

    // ── Degenerate inputs guard cleanly ──────────────────────────────────────

    TEST(RoughBergomiHybrid, ZeroPathsOrZeroStepsOrZeroTtmReturnsZeroWithoutCrashing)
    {
        RoughBergomiParams p;
        const auto xi0 = flat_curve(0.04);

        RoughBergomiSettings zero_paths = fast_settings();
        zero_paths.n_paths = 0;
        EXPECT_DOUBLE_EQ(rough_bergomi_price(kForward, 100.0, kTtm, kDiscount, p, xi0, true, zero_paths).price, 0.0);

        RoughBergomiSettings zero_steps = fast_settings();
        zero_steps.n_steps = 0;
        EXPECT_DOUBLE_EQ(rough_bergomi_price(kForward, 100.0, kTtm, kDiscount, p, xi0, true, zero_steps).price, 0.0);

        EXPECT_DOUBLE_EQ(rough_bergomi_price(kForward, 100.0, 0.0, kDiscount, p, xi0, true, fast_settings()).price, 0.0);
    }

} // namespace quantModeling
