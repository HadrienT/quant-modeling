#include <gtest/gtest.h>
#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/engines/mc/kernels/asian_bs.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include <cmath>
#include <memory>

namespace quantModeling
{

    // ============================================================================
    // Phase 3 — variance reduction for Asian options:
    // Sobol RQMC + Brownian bridge + geometric control variate.
    // ============================================================================

    class AsianVRTest : public ::testing::Test
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

        PricingResult price(OptionType type, AsianAverageType avg,
                            SamplerKind sampler, int paths, int seed,
                            bool control_variate = true) const
        {
            PricingContext ctx;
            ctx.model = model;
            ctx.settings.mc_paths = paths;
            ctx.settings.mc_seed = seed;
            ctx.settings.mc_sampler = sampler;
            ctx.settings.mc_control_variate = control_variate;

            auto payoff = std::make_shared<ArithmeticAsianPayoff>(type, K);
            auto exercise = std::make_shared<EuropeanExercise>(T);
            AsianOption option(payoff, exercise, avg, 1.0);

            BSEuroAsianMCEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        mc::AsianSpec make_spec() const
        {
            mc::AsianSpec s;
            s.S0 = S0;
            s.K = K;
            s.r = r;
            s.q = q;
            s.sigma = sigma;
            s.T = T;
            s.n_fixings = std::max(1, static_cast<int>(T * 252.0 + 0.5));
            s.df = std::exp(-r * T);
            return s;
        }

        Real S0, K, T, r, q, sigma;
        std::shared_ptr<const BlackScholesModel> model;
    };

    // The simulated geometric Asian (Sobol, no CV needed — it IS the control)
    // must match the discrete closed form. This validates both the path
    // construction (drift, bridge) and the Kemna-Vorst formula together.
    TEST_F(AsianVRTest, GeometricSobolMatchesClosedForm)
    {
        const Real exact = mc::discrete_geometric_asian_price(make_spec(), OptionType::Call);
        const PricingResult res = price(OptionType::Call, AsianAverageType::Geometric,
                                        SamplerKind::Sobol, 1 << 16, 42);

        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 5e-3));
    }

    TEST_F(AsianVRTest, GeometricSobolMatchesClosedFormPut)
    {
        const Real exact = mc::discrete_geometric_asian_price(make_spec(), OptionType::Put);
        const PricingResult res = price(OptionType::Put, AsianAverageType::Geometric,
                                        SamplerKind::Sobol, 1 << 16, 42);

        EXPECT_NEAR(res.npv, exact, std::max(4.0 * res.mc_std_error, 5e-3));
    }

    // Sobol + geometric CV must beat plain pseudo-random MC by a wide margin
    // at equal path budget.
    TEST_F(AsianVRTest, ControlVariateSlashesStdError)
    {
        const int paths = 1 << 16;
        const PricingResult prng = price(OptionType::Call, AsianAverageType::Arithmetic,
                                         SamplerKind::PseudoRandom, paths, 42);
        const PricingResult qmc = price(OptionType::Call, AsianAverageType::Arithmetic,
                                        SamplerKind::Sobol, paths, 42);

        EXPECT_GT(prng.mc_std_error, 0.0);
        EXPECT_GT(qmc.mc_std_error, 0.0);
        EXPECT_LT(qmc.mc_std_error, 0.2 * prng.mc_std_error)
            << "PRNG se=" << prng.mc_std_error << " QMC+CV se=" << qmc.mc_std_error;
    }

    // The CV estimator is unbiased: both estimators must agree within
    // combined confidence bounds.
    TEST_F(AsianVRTest, ControlVariateIsUnbiased)
    {
        const int paths = 1 << 17;
        const PricingResult prng = price(OptionType::Call, AsianAverageType::Arithmetic,
                                         SamplerKind::PseudoRandom, paths, 7);
        const PricingResult qmc = price(OptionType::Call, AsianAverageType::Arithmetic,
                                        SamplerKind::Sobol, paths, 7);

        const Real tol = 4.0 * std::sqrt(prng.mc_std_error * prng.mc_std_error +
                                         qmc.mc_std_error * qmc.mc_std_error);
        EXPECT_NEAR(qmc.npv, prng.npv, std::max(tol, 1e-3));
    }

    TEST_F(AsianVRTest, ControlVariateWorksForPut)
    {
        const int paths = 1 << 16;
        const PricingResult prng = price(OptionType::Put, AsianAverageType::Arithmetic,
                                         SamplerKind::PseudoRandom, paths, 42);
        const PricingResult qmc = price(OptionType::Put, AsianAverageType::Arithmetic,
                                        SamplerKind::Sobol, paths, 42);

        const Real tol = 4.0 * std::sqrt(prng.mc_std_error * prng.mc_std_error +
                                         qmc.mc_std_error * qmc.mc_std_error);
        EXPECT_NEAR(qmc.npv, prng.npv, std::max(tol, 1e-3));
        EXPECT_LT(qmc.mc_std_error, 0.5 * prng.mc_std_error);
    }

    // Pathwise delta on the QMC path must agree with the PRNG engine's delta.
    TEST_F(AsianVRTest, PathwiseDeltaConsistent)
    {
        const int paths = 1 << 16;
        const PricingResult prng = price(OptionType::Call, AsianAverageType::Arithmetic,
                                         SamplerKind::PseudoRandom, paths, 42);
        const PricingResult qmc = price(OptionType::Call, AsianAverageType::Arithmetic,
                                        SamplerKind::Sobol, paths, 42);

        ASSERT_TRUE(prng.greeks.delta.has_value());
        ASSERT_TRUE(qmc.greeks.delta.has_value());
        EXPECT_NEAR(*qmc.greeks.delta, *prng.greeks.delta, 0.02);
    }

    TEST_F(AsianVRTest, SobolPathIsReproducible)
    {
        const PricingResult a = price(OptionType::Call, AsianAverageType::Arithmetic,
                                      SamplerKind::Sobol, 1 << 14, 123);
        const PricingResult b = price(OptionType::Call, AsianAverageType::Arithmetic,
                                      SamplerKind::Sobol, 1 << 14, 123);

        EXPECT_DOUBLE_EQ(a.npv, b.npv);
        EXPECT_DOUBLE_EQ(a.mc_std_error, b.mc_std_error);
    }

    // Disabling the CV must still price correctly (plain RQMC).
    TEST_F(AsianVRTest, SobolWithoutCVStillUnbiased)
    {
        const int paths = 1 << 16;
        const PricingResult prng = price(OptionType::Call, AsianAverageType::Arithmetic,
                                         SamplerKind::PseudoRandom, paths, 42);
        const PricingResult qmc = price(OptionType::Call, AsianAverageType::Arithmetic,
                                        SamplerKind::Sobol, paths, 42,
                                        /*control_variate=*/false);

        const Real tol = 4.0 * std::sqrt(prng.mc_std_error * prng.mc_std_error +
                                         qmc.mc_std_error * qmc.mc_std_error);
        EXPECT_NEAR(qmc.npv, prng.npv, std::max(tol, 2e-3));
    }

} // namespace quantModeling
