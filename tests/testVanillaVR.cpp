#include <gtest/gtest.h>
#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include <cmath>
#include <memory>

namespace quantModeling
{

    // ============================================================================
    // Phase 3 — variance reduction for vanillas:
    // stratified sampling and drift-shift importance sampling.
    // ============================================================================

    class VanillaVRTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            S0 = 100.0;
            T = 1.0;
            r = 0.05;
            q = 0.02;
            sigma = 0.20;
            model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);
        }

        PricingResult price_mc(OptionType type, Real K, SamplerKind sampler,
                               int paths, int seed, bool importance_sampling,
                               bool antithetic = false) const
        {
            PricingContext ctx;
            ctx.model = model;
            ctx.settings.mc_paths = paths;
            ctx.settings.mc_seed = seed;
            ctx.settings.mc_antithetic = antithetic;
            ctx.settings.mc_sampler = sampler;
            ctx.settings.mc_importance_sampling = importance_sampling;

            auto payoff = std::make_shared<PlainVanillaPayoff>(type, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            VanillaOption option(payoff, exercise);

            BSEuroVanillaMCEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        PricingResult price_analytic(OptionType type, Real K) const
        {
            PricingContext ctx;
            ctx.model = model;
            auto payoff = std::make_shared<PlainVanillaPayoff>(type, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            VanillaOption option(payoff, exercise);
            BSEuroVanillaAnalyticEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        Real S0, T, r, q, sigma;
        std::shared_ptr<const BlackScholesModel> model;
    };

    // ---- Stratified sampling -------------------------------------------------

    TEST_F(VanillaVRTest, StratifiedIsUnbiased)
    {
        const Real exact = price_analytic(OptionType::Call, 100.0).npv;
        const PricingResult res = price_mc(OptionType::Call, 100.0,
                                           SamplerKind::Stratified, 1 << 16, 42,
                                           /*is=*/false);

        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-3));
    }

    TEST_F(VanillaVRTest, StratifiedBeatsPseudoRandom)
    {
        const int paths = 1 << 16;
        const PricingResult prng = price_mc(OptionType::Call, 100.0,
                                            SamplerKind::PseudoRandom, paths, 42,
                                            /*is=*/false);
        const PricingResult strat = price_mc(OptionType::Call, 100.0,
                                             SamplerKind::Stratified, paths, 42,
                                             /*is=*/false);

        EXPECT_GT(prng.mc_std_error, 0.0);
        EXPECT_GT(strat.mc_std_error, 0.0);
        EXPECT_LT(strat.mc_std_error, 0.2 * prng.mc_std_error)
            << "PRNG se=" << prng.mc_std_error
            << " stratified se=" << strat.mc_std_error;
    }

    TEST_F(VanillaVRTest, StratifiedGreeksMatchAnalytic)
    {
        const PricingResult exact = price_analytic(OptionType::Call, 100.0);
        const PricingResult strat = price_mc(OptionType::Call, 100.0,
                                             SamplerKind::Stratified, 1 << 16, 42,
                                             /*is=*/false);

        ASSERT_TRUE(strat.greeks.delta.has_value());
        ASSERT_TRUE(strat.greeks.vega.has_value());
        ASSERT_TRUE(strat.greeks.vega_std_error.has_value());
        EXPECT_NEAR(*strat.greeks.delta, *exact.greeks.delta, 5e-3);
        // LRM vega stays noisy under stratification (the score (z^2-1)/sigma
        // is not smoothed by stratifying z): compare within 4 sigma.
        EXPECT_NEAR(*strat.greeks.vega, *exact.greeks.vega,
                    std::max(4.0 * *strat.greeks.vega_std_error, 0.5));
    }

    TEST_F(VanillaVRTest, StratifiedIsReproducible)
    {
        const PricingResult a = price_mc(OptionType::Call, 100.0,
                                         SamplerKind::Stratified, 1 << 14, 7,
                                         /*is=*/false);
        const PricingResult b = price_mc(OptionType::Call, 100.0,
                                         SamplerKind::Stratified, 1 << 14, 7,
                                         /*is=*/false);
        EXPECT_DOUBLE_EQ(a.npv, b.npv);
    }

    // ---- Importance sampling (deep OTM) ---------------------------------------

    TEST_F(VanillaVRTest, ImportanceSamplingUnbiasedDeepOTMCall)
    {
        const Real K = 160.0; // ~3 sigma OTM
        const Real exact = price_analytic(OptionType::Call, K).npv;
        const PricingResult res = price_mc(OptionType::Call, K,
                                           SamplerKind::PseudoRandom, 1 << 16, 42,
                                           /*is=*/true);

        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-4));
    }

    TEST_F(VanillaVRTest, ImportanceSamplingUnbiasedDeepOTMPut)
    {
        const Real K = 60.0;
        const Real exact = price_analytic(OptionType::Put, K).npv;
        const PricingResult res = price_mc(OptionType::Put, K,
                                           SamplerKind::PseudoRandom, 1 << 16, 42,
                                           /*is=*/true);

        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-4));
    }

    TEST_F(VanillaVRTest, ImportanceSamplingSlashesOTMStdError)
    {
        const Real K = 160.0;
        const int paths = 1 << 16;
        const PricingResult plain = price_mc(OptionType::Call, K,
                                             SamplerKind::PseudoRandom, paths, 42,
                                             /*is=*/false);
        const PricingResult is = price_mc(OptionType::Call, K,
                                          SamplerKind::PseudoRandom, paths, 42,
                                          /*is=*/true);

        EXPECT_GT(plain.mc_std_error, 0.0);
        EXPECT_GT(is.mc_std_error, 0.0);
        EXPECT_LT(is.mc_std_error, 0.2 * plain.mc_std_error)
            << "plain se=" << plain.mc_std_error << " IS se=" << is.mc_std_error;
    }

    TEST_F(VanillaVRTest, ImportanceSamplingWithAntithetic)
    {
        const Real K = 160.0;
        const Real exact = price_analytic(OptionType::Call, K).npv;
        const PricingResult res = price_mc(OptionType::Call, K,
                                           SamplerKind::PseudoRandom, 1 << 16, 42,
                                           /*is=*/true, /*antithetic=*/true);
        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-4));
    }

    // ---- Composition: stratification × IS --------------------------------------

    TEST_F(VanillaVRTest, StratifiedPlusISUnbiasedAndTight)
    {
        const Real K = 160.0;
        const int paths = 1 << 16;
        const Real exact = price_analytic(OptionType::Call, K).npv;

        const PricingResult plain = price_mc(OptionType::Call, K,
                                             SamplerKind::PseudoRandom, paths, 42,
                                             /*is=*/false);
        const PricingResult both = price_mc(OptionType::Call, K,
                                            SamplerKind::Stratified, paths, 42,
                                            /*is=*/true);

        EXPECT_NEAR(both.npv, exact, std::max(4.0 * both.mc_std_error, 1e-4));
        EXPECT_LT(both.mc_std_error, 0.1 * plain.mc_std_error);
    }

} // namespace quantModeling
