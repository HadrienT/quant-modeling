#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/gaussian_source.hpp"
#include "quantModeling/utils/inverse_normal.hpp"

namespace quantModeling
{
    namespace
    {
        // Standard normal CDF for round-trip checks
        Real norm_cdf(Real x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
    } // namespace

    // ---------------------------------------------------------------------
    // inverse_normal_cdf
    // ---------------------------------------------------------------------

    TEST(InverseNormal, KnownQuantiles)
    {
        EXPECT_NEAR(inverse_normal_cdf(0.5), 0.0, 1e-14);
        // Classic two-sided quantiles
        EXPECT_NEAR(inverse_normal_cdf(0.975), 1.9599639845400545, 1e-12);
        EXPECT_NEAR(inverse_normal_cdf(0.025), -1.9599639845400545, 1e-12);
        EXPECT_NEAR(inverse_normal_cdf(0.99), 2.3263478740408408, 1e-12);
        EXPECT_NEAR(inverse_normal_cdf(0.84134474606854293), 1.0, 1e-12);
    }

    TEST(InverseNormal, RoundTripFullRange)
    {
        // Φ(Φ⁻¹(p)) == p over central range and both tails
        for (double p = 1e-10; p < 1.0; p = (p < 0.5) ? p * 3.0 : 1.0 - (1.0 - p) / 3.0)
        {
            const double x = inverse_normal_cdf(p);
            EXPECT_NEAR(norm_cdf(x), p, 1e-12 + 1e-9 * p) << "p = " << p;
            if (p > 0.999999)
                break;
        }
    }

    TEST(InverseNormal, Symmetry)
    {
        for (double p : {0.01, 0.1, 0.3, 0.45})
        {
            EXPECT_NEAR(inverse_normal_cdf(p), -inverse_normal_cdf(1.0 - p), 1e-11);
        }
    }

    // ---------------------------------------------------------------------
    // WelfordAccumulator
    // ---------------------------------------------------------------------

    TEST(Welford, MeanVarianceAndMerge)
    {
        const double xs[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};

        WelfordAccumulator all;
        WelfordAccumulator left, right;
        for (int i = 0; i < 8; ++i)
        {
            all.add(xs[i]);
            (i < 3 ? left : right).add(xs[i]);
        }

        EXPECT_NEAR(all.mean, 4.5, 1e-14);
        EXPECT_NEAR(all.variance(), 6.0, 1e-14); // unbiased var of 1..8

        left.merge(right);
        EXPECT_EQ(left.n, all.n);
        EXPECT_NEAR(left.mean, all.mean, 1e-14);
        EXPECT_NEAR(left.m2, all.m2, 1e-12);
    }

    // ---------------------------------------------------------------------
    // InverseNormal gaussian source through the vanilla MC engine
    // ---------------------------------------------------------------------

    namespace
    {
        PricingResult priceWithSource(GaussianKind kind, OptionType type)
        {
            const Real S0 = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.20;

            PricingContext ctx;
            ctx.model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);
            ctx.settings.mc_paths = 500'000;
            ctx.settings.mc_seed = 42;
            ctx.settings.mc_antithetic = true;
            ctx.settings.mc_gaussian = kind;

            VanillaOption opt(std::make_shared<PlainVanillaPayoff>(type, K),
                              std::make_shared<EuropeanExercise>(T));

            BSEuroVanillaMCEngine engine(ctx);
            opt.accept(engine);
            return engine.results();
        }
    } // namespace

    TEST(GaussianSourceMC, InverseNormalMatchesBoxMullerWithinStdError)
    {
        for (OptionType type : {OptionType::Call, OptionType::Put})
        {
            const PricingResult bm = priceWithSource(GaussianKind::BoxMuller, type);
            const PricingResult in = priceWithSource(GaussianKind::InverseNormal, type);

            // Different draw sequences, same distribution: prices agree within
            // combined 4-sigma MC uncertainty.
            const Real tol = 4.0 * std::sqrt(bm.mc_std_error * bm.mc_std_error +
                                             in.mc_std_error * in.mc_std_error);
            EXPECT_LE(std::abs(bm.npv - in.npv), tol);
            EXPECT_GT(in.mc_std_error, 0.0);
        }
    }

    TEST(GaussianSourceMC, InverseNormalReproducibleWithFixedSeed)
    {
        const PricingResult a = priceWithSource(GaussianKind::InverseNormal, OptionType::Call);
        const PricingResult b = priceWithSource(GaussianKind::InverseNormal, OptionType::Call);
        EXPECT_NEAR(a.npv, b.npv, 1e-12);
    }

} // namespace quantModeling
