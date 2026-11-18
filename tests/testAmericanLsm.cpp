#include <gtest/gtest.h>

#include "quantModeling/core/types.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/pricers/script_builders.hpp"

#include <cmath>
#include <string>

// The American option by Longstaff-Schwartz on a script (#313): the route the
// site prices, a Bermudan on 50 dates a year. The binomial tree stays as its
// oracle. An estimated exercise rule can only lose value against the optimal
// one (a lower bound), and a Bermudan is worth less than the American: the
// price must lie between the European and the tree, close to the tree.

namespace quantModeling
{
    namespace
    {
        constexpr Real S0 = 100.0, K = 100.0, T = 1.0, r = 0.05, sigma = 0.20;

        AmericanVanillaBSInput american(bool is_call, Real q)
        {
            AmericanVanillaBSInput in{S0, K, T, r, q, sigma, is_call};
            in.tree_steps = 2000;
            in.n_paths = 100000;
            in.seed = 3;
            in.compute_greeks = false;
            return in;
        }

        PricingResult price(const AmericanVanillaBSInput &in, EngineKind engine)
        {
            return default_registry().price(PricingRequest{InstrumentKind::EquityAmericanVanillaOption,
                                                           ModelKind::BlackScholes, engine, PricingInput{in}});
        }

        Real european(bool is_call, Real q)
        {
            VanillaBSInput in{S0, K, T, r, q, sigma, is_call};
            return default_registry()
                .price(PricingRequest{InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes,
                                      EngineKind::Analytic, PricingInput{in}})
                .npv;
        }

        void expect_between_european_and_tree(bool is_call, Real q)
        {
            const AmericanVanillaBSInput in = american(is_call, q);
            const PricingResult mc = price(in, EngineKind::MonteCarlo);
            const Real tree = price(in, EngineKind::BinomialTree).npv;
            const Real se = mc.mc_std_error;
            const Real euro = european(is_call, q);
            ASSERT_GT(tree - euro, 0.05) << "the case must have an early-exercise premium to test";
            EXPECT_GT(mc.npv, euro + 4.0 * se) << "the rule must capture the early-exercise premium";
            EXPECT_LT(mc.npv, tree + 3.0 * se) << "a lower bound cannot exceed the American";
            // The Bermudan gap and the estimated rule: a few tenths of a
            // percent (the pull request and WP 26 §7 give the figures).
            EXPECT_GT(mc.npv, 0.99 * tree);
            EXPECT_NE(mc.diagnostics.find("Longstaff-Schwartz"), std::string::npos) << mc.diagnostics;
        }
    } // namespace

    TEST(AmericanLsm, PutLiesBetweenTheEuropeanAndTheTree)
    {
        expect_between_european_and_tree(false, 0.0);
    }

    TEST(AmericanLsm, PutWithDividendLiesBetweenTheEuropeanAndTheTree)
    {
        expect_between_european_and_tree(false, 0.04);
    }

    TEST(AmericanLsm, CallWithDividendLiesBetweenTheEuropeanAndTheTree)
    {
        expect_between_european_and_tree(true, 0.08);
    }

    // A call whose early-exercise premium is tiny (r 5 %, q 4 %: 0.015 on
    // 8.10): exercising is optimal only far above the strike (S > rK/q near
    // the expiry), and the estimated rule, without the gate on the forward,
    // exercised below it and priced under the European (8.081).
    TEST(AmericanLsm, CallWithASmallPremiumIsNotBelowTheEuropean)
    {
        const AmericanVanillaBSInput in = american(true, 0.04);
        const PricingResult mc = price(in, EngineKind::MonteCarlo);
        const Real tree = price(in, EngineKind::BinomialTree).npv;
        EXPECT_GT(mc.npv, european(true, 0.04) - 2.0 * mc.mc_std_error);
        EXPECT_LT(mc.npv, tree + 3.0 * mc.mc_std_error);
    }

    // Merton (1973): with no dividend and r >= 0 the call is never exercised
    // early, so the route prices the European -- exactly, where an estimated
    // rule would lose value exercising on noise.
    TEST(AmericanLsm, CallWithoutDividendIsTheEuropean)
    {
        const PricingResult mc = price(american(true, 0.0), EngineKind::MonteCarlo);
        EXPECT_NEAR(mc.npv, european(true, 0.0), 1e-3);
        EXPECT_NE(mc.diagnostics.find("Merton"), std::string::npos) << mc.diagnostics;
    }

    // The greeks under the fitted rule (held fixed, the envelope argument):
    // delta from the adjoint, as the tree's within 2 %.
    TEST(AmericanLsm, DeltaMatchesTheTree)
    {
        AmericanVanillaBSInput in = american(false, 0.0);
        in.compute_greeks = true;
        const PricingResult mc = price(in, EngineKind::MonteCarlo);
        const PricingResult tree = price(in, EngineKind::BinomialTree);
        ASSERT_TRUE(mc.greeks.delta && tree.greeks.delta);
        EXPECT_NEAR(*mc.greeks.delta, *tree.greeks.delta, 0.02 * std::abs(*tree.greeks.delta));
        EXPECT_LT(*mc.greeks.delta, 0.0);
        ASSERT_TRUE(mc.greeks.vega && tree.greeks.vega);
        EXPECT_NEAR(*mc.greeks.vega, *tree.greeks.vega, 0.02 * *tree.greeks.vega);
    }

    // The script: exercise gated in the money and above the forward held to
    // the expiry, nothing on the expiry but the payment of a live option.
    TEST(AmericanLsm, ScriptGatesExerciseInTheMoney)
    {
        const std::string s = script_builders::american(false, 100.0, {24, 48, 72}, 0.02, 72.0 / 8760.0);
        EXPECT_NE(s.find("spot() < 100 and max(100 - spot(), 0) > 100 * df("), std::string::npos) << s;
        EXPECT_NE(s.find("and exercise(spot())"), std::string::npos) << s;
        const std::size_t expiry = s.find((script_builders::valuation() + 72).to_iso());
        ASSERT_NE(expiry, std::string::npos) << s;
        const std::string last_event = s.substr(expiry);
        EXPECT_EQ(last_event.find("exercise"), std::string::npos) << s;
        EXPECT_NE(last_event.find("if done = 0 then"), std::string::npos) << s;
    }

} // namespace quantModeling
