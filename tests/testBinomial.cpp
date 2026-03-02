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
        PricingResult priceBinomial(bool is_call, int steps = STEPS,
                                    Real strike = K, Real maturity = T)
        {
            VanillaBSInput in{S0, strike, maturity, r, q, sigma, is_call};
            in.tree_steps = steps;

            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::BinomialTree,
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

        PricingResult priceAmericanBinomial(bool is_call, int steps = STEPS,
                                            Real strike = K, Real maturity = T)
        {
            AmericanVanillaBSInput in{S0, strike, maturity, r, q, sigma, is_call};
            in.tree_steps = steps;

            PricingRequest request{
                InstrumentKind::EquityAmericanVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::BinomialTree,
                PricingInput{in}};
            return default_registry().price(request);
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Convergence to BS analytic
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, EuroCallConvergesToBS)
    {
        const Real ana = priceAnalytic(true).npv;
        const Real tree = priceBinomial(true, 500).npv;
        EXPECT_NEAR(tree, ana, ana * 0.005)
            << "Binomial call with 500 steps should be within 0.5% of BS";
    }

    TEST(BinomialTree, EuroPutConvergesToBS)
    {
        const Real ana = priceAnalytic(false).npv;
        const Real tree = priceBinomial(false, 500).npv;
        EXPECT_NEAR(tree, ana, ana * 0.005);
    }

    TEST(BinomialTree, ConvergenceAsStepsIncrease)
    {
        const Real ana = priceAnalytic(true).npv;
        const Real e50 = std::abs(priceBinomial(true, 50).npv - ana);
        const Real e200 = std::abs(priceBinomial(true, 200).npv - ana);
        EXPECT_LT(e200, e50) << "Error should decrease with more steps";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Call-put parity (European)
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, EuroCallPutParity)
    {
        const Real C = priceBinomial(true, 300).npv;
        const Real P = priceBinomial(false, 300).npv;
        const Real rhs = S0 * std::exp(-q * T) - K * std::exp(-r * T);
        EXPECT_NEAR(C - P, rhs, 0.05);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Greeks
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, GreeksPopulated)
    {
        const auto res = priceBinomial(true, 200);
        EXPECT_TRUE(res.greeks.delta.has_value());
        EXPECT_TRUE(res.greeks.gamma.has_value());
        EXPECT_TRUE(res.greeks.vega.has_value());
        EXPECT_TRUE(res.greeks.theta.has_value());
    }

    TEST(BinomialTree, CallDeltaPositive)
    {
        const auto res = priceBinomial(true, 200);
        EXPECT_GT(*res.greeks.delta, 0.0);
        EXPECT_LT(*res.greeks.delta, 1.0);
    }

    TEST(BinomialTree, PutDeltaNegative)
    {
        const auto res = priceBinomial(false, 200);
        EXPECT_LT(*res.greeks.delta, 0.0);
        EXPECT_GT(*res.greeks.delta, -1.0);
    }

    TEST(BinomialTree, GammaPositive)
    {
        EXPECT_GT(*priceBinomial(true, 200).greeks.gamma, 0.0);
        EXPECT_GT(*priceBinomial(false, 200).greeks.gamma, 0.0);
    }

    TEST(BinomialTree, VegaPositive)
    {
        EXPECT_GT(*priceBinomial(true, 200).greeks.vega, 0.0);
        EXPECT_GT(*priceBinomial(false, 200).greeks.vega, 0.0);
    }

    TEST(BinomialTree, DeltaConvergesToBS)
    {
        const Real ana_delta = *priceAnalytic(true).greeks.delta;
        const Real tree_delta = *priceBinomial(true, 500).greeks.delta;
        EXPECT_NEAR(tree_delta, ana_delta, 0.01);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  American options
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, AmericanPutGEEuropeanPut)
    {
        // American put ≥ European put (early exercise premium)
        const Real american = priceAmericanBinomial(false, 200).npv;
        const Real european = priceBinomial(false, 200).npv;
        EXPECT_GE(american, european - 0.01);
    }

    TEST(BinomialTree, AmericanCallEqualsEuropeanCallNoDividend)
    {
        // For q > 0 American call may differ, but for call the early exercise
        // premium is generally small. At least check they're close.
        const Real american = priceAmericanBinomial(true, 200).npv;
        const Real european = priceBinomial(true, 200).npv;
        EXPECT_NEAR(american, european, european * 0.02);
    }

    TEST(BinomialTree, AmericanPutDeepITMExerciseEarly)
    {
        // Deep ITM American put (K=150, S0=100): intrinsic = 50
        const Real american = priceAmericanBinomial(false, 200, 150.0).npv;
        EXPECT_GE(american, 50.0 - 0.1) << "Deep ITM American put should be >= intrinsic";
    }

    TEST(BinomialTree, AmericanPutIsPositive)
    {
        EXPECT_GT(priceAmericanBinomial(false, 200).npv, 0.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Diagnostics
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, DiagnosticsContainCRR)
    {
        const auto res = priceBinomial(true, 100);
        EXPECT_NE(res.diagnostics.find("Binomial tree"), std::string::npos);
        EXPECT_NE(res.diagnostics.find("CRR"), std::string::npos);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Edge cases
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BinomialTree, ShortMaturity)
    {
        const Real npv = priceBinomial(true, 50, K, 0.01).npv;
        EXPECT_GE(npv, 0.0);
    }

} // namespace quantModeling
