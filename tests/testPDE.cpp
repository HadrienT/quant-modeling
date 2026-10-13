#include <gtest/gtest.h>

#include "quantModeling/instruments/base.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {

        constexpr Real S0 = 100.0;
        constexpr Real K = 100.0;
        constexpr Real T = 1.0;
        constexpr Real r = 0.05;
        constexpr Real q = 0.02;
        constexpr Real sigma = 0.20;

        // ── Helpers ─────────────────────────────────────────────────────────
        PricingResult pricePDE(bool is_call, int M = 200, int N = 200,
                               Real strike = K, Real maturity = T)
        {
            VanillaBSInput in{S0, strike, maturity, r, q, sigma, is_call};
            in.pde_space_steps = M;
            in.pde_time_steps = N;

            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::PDEFiniteDifference,
                PricingInput{in}};
            return default_registry().price(request);
        }

        PricingResult priceAnalytic(bool is_call)
        {
            VanillaBSInput in{S0, K, T, r, q, sigma, is_call};
            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::Analytic,
                PricingInput{in}};
            return default_registry().price(request);
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Convergence to BS analytic
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, CallConvergesToBS)
    {
        const Real ana = priceAnalytic(true).npv;
        EXPECT_NEAR(pricePDE(true, 100, 100).npv, ana, 1e-2);
        EXPECT_NEAR(pricePDE(true, 400, 400).npv, ana, 1e-3);
    }

    TEST(PDEVanilla, PutConvergesToBS)
    {
        const Real ana = priceAnalytic(false).npv;
        EXPECT_NEAR(pricePDE(false, 100, 100).npv, ana, 1e-2);
        EXPECT_NEAR(pricePDE(false, 400, 400).npv, ana, 1e-3);
    }

    TEST(PDEVanilla, ErrorFallsAtSecondOrderAsTheGridIsRefined)
    {
        // Crank-Nicolson is second order in time and in space: halving both
        // steps divides the error by about four. A scheme that applied more
        // than half a step of the operator on the right-hand side would not
        // converge at all, and would blow up on the finer grids.
        const Real ana = priceAnalytic(true).npv;
        Real previous = std::abs(pricePDE(true, 50, 50).npv - ana);
        for (const int steps : {100, 200, 400, 800})
        {
            const Real error = std::abs(pricePDE(true, steps, steps).npv - ana);
            const Real order = std::log2(previous / error);
            EXPECT_GT(order, 1.7) << "steps=" << steps << " error=" << error;
            EXPECT_LT(order, 2.3) << "steps=" << steps << " error=" << error;
            previous = error;
        }
    }

    TEST(PDEVanilla, StaysStableWhenSpaceIsMuchFinerThanTime)
    {
        // dt / dx^2 = 400 here: an explicit step would be far outside its
        // stability region.
        const Real ana = priceAnalytic(true).npv;
        EXPECT_NEAR(pricePDE(true, 2000, 50).npv, ana, 5e-2);
    }

    TEST(PDEVanilla, PriceIsFinite)
    {
        const Real pde = pricePDE(true, 100, 100).npv;
        EXPECT_TRUE(std::isfinite(pde));
        EXPECT_GT(pde, 0.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Call-put parity
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, CallPutParity)
    {
        const Real C = pricePDE(true, 100, 100).npv;
        const Real P = pricePDE(false, 100, 100).npv;
        const Real rhs = S0 * std::exp(-q * T) - K * std::exp(-r * T);
        EXPECT_NEAR(C - P, rhs, 1e-2);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Greeks
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, DeltaPopulated)
    {
        const auto res = pricePDE(true, 100, 100);
        EXPECT_TRUE(res.greeks.delta.has_value());
        EXPECT_TRUE(res.greeks.gamma.has_value());
    }

    TEST(PDEVanilla, DeltaAndGammaMatchBS)
    {
        for (const bool is_call : {true, false})
        {
            const auto ana = priceAnalytic(is_call);
            const auto pde = pricePDE(is_call, 400, 400);
            EXPECT_NEAR(*pde.greeks.delta, *ana.greeks.delta, 1e-3) << "is_call=" << is_call;
            EXPECT_NEAR(*pde.greeks.gamma, *ana.greeks.gamma, 1e-4) << "is_call=" << is_call;
        }
    }

    TEST(PDEVanilla, GammaPopulated)
    {
        const auto res = pricePDE(true, 100, 100);
        EXPECT_TRUE(res.greeks.gamma.has_value());
        EXPECT_TRUE(std::isfinite(*res.greeks.gamma));
    }

    TEST(PDEVanilla, DeltaIsFinite)
    {
        const auto res = pricePDE(true, 100, 100);
        EXPECT_TRUE(std::isfinite(*res.greeks.delta));
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Basic properties
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, CallPricePositive)
    {
        EXPECT_GT(pricePDE(true).npv, 0.0);
    }

    TEST(PDEVanilla, PutPricePositive)
    {
        EXPECT_GT(pricePDE(false).npv, 0.0);
    }

    TEST(PDEVanilla, CallPriceLessThanSpot)
    {
        EXPECT_LT(pricePDE(true, 100, 100).npv, S0 * 1.5);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  OTM / ITM sanity
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, DeepITMCallNearIntrinsic)
    {
        // K=50, S0=100 → call should be substantial
        const Real npv = pricePDE(true, 100, 100, 50.0).npv;
        EXPECT_GT(npv, 40.0) << "Deep ITM call should be worth at least ~40";
    }

    TEST(PDEVanilla, DeepOTMCallNearZero)
    {
        const Real npv = pricePDE(true, 100, 100, 200.0).npv;
        EXPECT_LT(npv, 5.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Diagnostics
    // ─────────────────────────────────────────────────────────────────────────

    TEST(PDEVanilla, DiagnosticsContainPDE)
    {
        const auto res = pricePDE(true);
        EXPECT_NE(res.diagnostics.find("PDE"), std::string::npos);
        EXPECT_NE(res.diagnostics.find("Crank-Nicolson"), std::string::npos);
    }

} // namespace quantModeling
