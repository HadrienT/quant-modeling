#include <gtest/gtest.h>
#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/analytic/digital.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/lookback.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>
#include <memory>

namespace quantModeling
{

    // ============================================================================
    // Phase 3 — payoff smoothing:
    //  - conditional MC for digitals (indicator -> Phi),
    //  (the barriers' survival probabilities moved to the scripts: tests/testBarrierMonitoring.cpp),
    //  - bridge-sampled extrema for lookbacks (discretisation bias removal).
    // ============================================================================

    class SmoothingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            S0 = 100.0;
            K = 100.0;
            T = 1.0;
            r = 0.05;
            q = 0.02;
            sigma = 0.20;
            model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);
        }

        PricingContext make_ctx(int paths, int seed, bool cmc = false,
                                bool bridge_extrema = false) const
        {
            PricingContext ctx;
            ctx.model = model;
            ctx.settings.mc_paths = paths;
            ctx.settings.mc_seed = seed;
            ctx.settings.mc_antithetic = false;
            ctx.settings.mc_cmc = cmc;
            ctx.settings.mc_bridge_extrema = bridge_extrema;
            return ctx;
        }

        // --- digitals ---
        PricingResult price_digital_mc(OptionType type, DigitalPayoffType pt,
                                       int paths, int seed, bool cmc) const
        {
            const PricingContext ctx = make_ctx(paths, seed, cmc);
            auto payoff = std::make_shared<PlainVanillaPayoff>(type, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            DigitalOption option(payoff, exercise, pt, /*cash=*/1.0);
            BSEuroVanillaMCEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        PricingResult price_digital_analytic(OptionType type, DigitalPayoffType pt) const
        {
            const PricingContext ctx = make_ctx(0, 0);
            auto payoff = std::make_shared<PlainVanillaPayoff>(type, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            DigitalOption option(payoff, exercise, pt, /*cash=*/1.0);
            BSDigitalAnalyticEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        // --- lookbacks ---
        PricingResult price_lookback_mc(int n_steps, int paths, int seed,
                                        bool bridge) const
        {
            const PricingContext ctx = make_ctx(paths, seed, false, bridge);
            // Floating-strike put: payoff = S_max - S_T (driven by the max).
            auto payoff = std::make_shared<PlainVanillaPayoff>(OptionType::Put, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            LookbackOption option(payoff, exercise, LookbackStyle::FloatingStrike,
                                  LookbackExtremum::Maximum);
            option.n_steps = n_steps;
            BSEuroLookbackMCEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        Real S0, K, T, r, q, sigma;
        std::shared_ptr<const BlackScholesModel> model;
    };

    // ---- Digitals --------------------------------------------------------------

    TEST_F(SmoothingTest, PlainDigitalMCMatchesAnalytic)
    {
        const Real exact = price_digital_analytic(OptionType::Call,
                                                  DigitalPayoffType::CashOrNothing)
                               .npv;
        const PricingResult res = price_digital_mc(OptionType::Call,
                                                   DigitalPayoffType::CashOrNothing,
                                                   1 << 17, 42, /*cmc=*/false);
        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-4));
    }

    TEST_F(SmoothingTest, CMCDigitalUnbiased)
    {
        const Real exact = price_digital_analytic(OptionType::Call,
                                                  DigitalPayoffType::CashOrNothing)
                               .npv;
        const PricingResult res = price_digital_mc(OptionType::Call,
                                                   DigitalPayoffType::CashOrNothing,
                                                   1 << 17, 42, /*cmc=*/true);
        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-4));
    }

    TEST_F(SmoothingTest, CMCDigitalReducesVariance)
    {
        const int paths = 1 << 16;
        const PricingResult plain = price_digital_mc(OptionType::Call,
                                                     DigitalPayoffType::CashOrNothing,
                                                     paths, 42, false);
        const PricingResult cmc = price_digital_mc(OptionType::Call,
                                                   DigitalPayoffType::CashOrNothing,
                                                   paths, 42, true);
        EXPECT_GT(plain.mc_std_error, 0.0);
        EXPECT_GT(cmc.mc_std_error, 0.0);
        EXPECT_LT(cmc.mc_std_error, 0.8 * plain.mc_std_error)
            << "plain se=" << plain.mc_std_error << " CMC se=" << cmc.mc_std_error;
    }

    TEST_F(SmoothingTest, CMCDigitalDeltaMatchesClosedForm)
    {
        // Analytic cash-or-nothing call delta: cash e^{-rT} phi(d2) / (S0 sigma sqrt(T)).
        const Real sqrtT = std::sqrt(T);
        const Real d2 = (std::log(S0 / K) + (r - q - 0.5 * sigma * sigma) * T) /
                        (sigma * sqrtT);
        const Real exact_delta = std::exp(-r * T) * norm_pdf(d2) / (S0 * sigma * sqrtT);

        const PricingResult res = price_digital_mc(OptionType::Call,
                                                   DigitalPayoffType::CashOrNothing,
                                                   1 << 17, 42, /*cmc=*/true);
        ASSERT_TRUE(res.greeks.delta.has_value());
        ASSERT_TRUE(res.greeks.delta_std_error.has_value());
        EXPECT_NEAR(*res.greeks.delta, exact_delta,
                    std::max(4.0 * *res.greeks.delta_std_error, 1e-5));
    }

    TEST_F(SmoothingTest, CMCDigitalAssetOrNothingUnbiased)
    {
        const Real exact = price_digital_analytic(OptionType::Put,
                                                  DigitalPayoffType::AssetOrNothing)
                               .npv;
        const PricingResult res = price_digital_mc(OptionType::Put,
                                                   DigitalPayoffType::AssetOrNothing,
                                                   1 << 17, 42, /*cmc=*/true);
        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 1e-3));
    }

    // ---- Lookbacks -------------------------------------------------------------

    TEST_F(SmoothingTest, BridgeMaxIncreasesLookbackPrice)
    {
        // The sampled bridge max is >= the discrete max path by path, so the
        // floating-put price (S_max - S_T) must strictly increase.
        const PricingResult discrete = price_lookback_mc(50, 20'000, 42, false);
        const PricingResult bridged = price_lookback_mc(50, 20'000, 42, true);
        EXPECT_GT(bridged.npv, discrete.npv);
    }

    TEST_F(SmoothingTest, BridgeMaxRemovesDiscretisationBias)
    {
        // Reference: discrete monitoring with a very fine grid (n=2000) is
        // close to the continuous price (bias ~ sqrt(dt)). The bridged coarse
        // grid (n=50) must land far closer to it than the discrete coarse grid.
        const PricingResult ref = price_lookback_mc(2000, 40'000, 99, false);
        const PricingResult coarse_disc = price_lookback_mc(50, 40'000, 42, false);
        const PricingResult coarse_bridge = price_lookback_mc(50, 40'000, 42, true);

        const Real gap_disc = std::abs(coarse_disc.npv - ref.npv);
        const Real gap_bridge = std::abs(coarse_bridge.npv - ref.npv);
        EXPECT_LT(gap_bridge, 0.5 * gap_disc)
            << "ref=" << ref.npv << " disc(50)=" << coarse_disc.npv
            << " bridge(50)=" << coarse_bridge.npv;
    }

    TEST_F(SmoothingTest, BridgeLookbackReproducible)
    {
        const PricingResult a = price_lookback_mc(50, 10'000, 123, true);
        const PricingResult b = price_lookback_mc(50, 10'000, 123, true);
        EXPECT_DOUBLE_EQ(a.npv, b.npv);
    }

} // namespace quantModeling
