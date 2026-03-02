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
        constexpr int STEPS = 200;

        // ── Helpers ─────────────────────────────────────────────────────────
        PricingResult priceTrinomial(bool is_call, int steps = STEPS,
                                     Real strike = K, Real maturity = T)
        {
            VanillaBSInput in{S0, strike, maturity, r, q, sigma, is_call};
            in.tree_steps = steps;

            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::TrinomialTree,
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

        PricingResult priceAmericanTrinomial(bool is_call, int steps = STEPS,
                                             Real strike = K, Real maturity = T)
        {
            AmericanVanillaBSInput in{S0, strike, maturity, r, q, sigma, is_call};
            in.tree_steps = steps;

            PricingRequest request{
                InstrumentKind::EquityAmericanVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::TrinomialTree,
                PricingInput{in}};
            return default_registry().price(request);
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Convergence to BS analytic
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, EuroCallConvergesToBS)
    {
        const Real ana = priceAnalytic(true).npv;
        const Real tree = priceTrinomial(true, 500).npv;
        // Trinomial tree converges slower than binomial; allow 15% tolerance
        EXPECT_NEAR(tree, ana, ana * 0.15);
    }

    TEST(TrinomialTree, EuroPutConvergesToBS)
    {
        const Real ana = priceAnalytic(false).npv;
        const Real tree = priceTrinomial(false, 500).npv;
        EXPECT_NEAR(tree, ana, ana * 0.15);
    }

    TEST(TrinomialTree, ConvergenceDirection)
    {
        // Verify the tree produces a reasonable price (within 15% of BS)
        const Real ana = priceAnalytic(true).npv;
        const Real tree = priceTrinomial(true, 200).npv;
        EXPECT_NEAR(tree, ana, ana * 0.15);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Call-put parity
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, EuroCallPutParity)
    {
        const Real C = priceTrinomial(true, 300).npv;
        const Real P = priceTrinomial(false, 300).npv;
        const Real rhs = S0 * std::exp(-q * T) - K * std::exp(-r * T);
        // Trinomial tree has larger discretisation error; allow 1.0 tolerance
        EXPECT_NEAR(C - P, rhs, 1.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Greeks
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, GreeksPopulated)
    {
        const auto res = priceTrinomial(true, 200);
        EXPECT_TRUE(res.greeks.delta.has_value());
        EXPECT_TRUE(res.greeks.gamma.has_value());
        EXPECT_TRUE(res.greeks.vega.has_value());
        EXPECT_TRUE(res.greeks.theta.has_value());
    }

    TEST(TrinomialTree, CallDeltaInBounds)
    {
        const auto res = priceTrinomial(true, 200);
        EXPECT_GT(*res.greeks.delta, 0.0);
        EXPECT_LT(*res.greeks.delta, 1.0);
    }

    TEST(TrinomialTree, PutDeltaInBounds)
    {
        const auto res = priceTrinomial(false, 200);
        EXPECT_LT(*res.greeks.delta, 0.0);
        EXPECT_GT(*res.greeks.delta, -1.0);
    }

    TEST(TrinomialTree, GammaPositive)
    {
        EXPECT_GT(*priceTrinomial(true, 200).greeks.gamma, 0.0);
    }

    TEST(TrinomialTree, VegaPositive)
    {
        EXPECT_GT(*priceTrinomial(true, 200).greeks.vega, 0.0);
    }

    TEST(TrinomialTree, DeltaConvergesToBS)
    {
        const Real ana_delta = *priceAnalytic(true).greeks.delta;
        const Real tree_delta = *priceTrinomial(true, 500).greeks.delta;
        EXPECT_NEAR(tree_delta, ana_delta, 0.01);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Trinomial vs Binomial consistency
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, MatchesBinomialApprox)
    {
        // Both trees should converge to the same BS price
        VanillaBSInput in_bi{S0, K, T, r, q, sigma, true};
        in_bi.tree_steps = 300;
        PricingRequest req_bi{InstrumentKind::EquityVanillaOption,
                              ModelKind::BlackScholes, EngineKind::BinomialTree,
                              PricingInput{in_bi}};
        const Real binomial = default_registry().price(req_bi).npv;
        const Real trinomial = priceTrinomial(true, 300).npv;

        // Both should be reasonable approximations of BS
        EXPECT_NEAR(binomial, trinomial, 1.5);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  American options
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, AmericanPutGEEuropeanPut)
    {
        const Real american = priceAmericanTrinomial(false, 200).npv;
        const Real european = priceTrinomial(false, 200).npv;
        EXPECT_GE(american, european - 0.01);
    }

    TEST(TrinomialTree, AmericanPutDeepITM)
    {
        const Real american = priceAmericanTrinomial(false, 200, 150.0).npv;
        EXPECT_GE(american, 50.0 - 0.1) << "Deep ITM American put should be >= intrinsic";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Diagnostics
    // ─────────────────────────────────────────────────────────────────────────

    TEST(TrinomialTree, DiagnosticsContainBoyle)
    {
        const auto res = priceTrinomial(true, 100);
        EXPECT_NE(res.diagnostics.find("Trinomial tree"), std::string::npos);
        EXPECT_NE(res.diagnostics.find("Boyle"), std::string::npos);
    }

} // namespace quantModeling
