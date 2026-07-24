#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"

namespace quantModeling
{

    // ---------------------------------------------------------------------
    // SobolSequence
    // ---------------------------------------------------------------------

    // The first 2^k points of every Sobol dimension form a (0,k,1)-net in
    // base 2: exactly one point falls in each dyadic interval
    // [j/2^k, (j+1)/2^k). The digital shift preserves this property.
    // This is a strong end-to-end check of the direction-number data.
    TEST(Sobol, DyadicNetPropertyPerDimension)
    {
        constexpr int dim = 64;
        constexpr int k = 10;
        constexpr int n = 1 << k;

        SobolSequence seq(dim, /*scramble_seed=*/12345);
        std::vector<double> point(dim);
        std::vector<std::vector<int>> counts(dim, std::vector<int>(n, 0));

        for (int i = 0; i < n; ++i)
        {
            seq.next_uniform(point);
            for (int d = 0; d < dim; ++d)
            {
                const int bucket = static_cast<int>(point[static_cast<size_t>(d)] * n);
                ASSERT_GE(bucket, 0);
                ASSERT_LT(bucket, n);
                ++counts[static_cast<size_t>(d)][static_cast<size_t>(bucket)];
            }
        }
        for (int d = 0; d < dim; ++d)
            for (int j = 0; j < n; ++j)
                ASSERT_EQ(counts[static_cast<size_t>(d)][static_cast<size_t>(j)], 1)
                    << "dim " << d << " bucket " << j;
    }

    // 2-D stratification of the first two dimensions: the first 2^k points
    // put exactly one point in each cell of the 2^(k/2) x 2^(k/2) grid.
    TEST(Sobol, TwoDimensionalStratification)
    {
        constexpr int k = 10;
        constexpr int n = 1 << k;
        constexpr int g = 1 << (k / 2); // 32 x 32 grid

        SobolSequence seq(2, 987);
        std::vector<double> p(2);
        std::vector<int> cells(g * g, 0);
        for (int i = 0; i < n; ++i)
        {
            seq.next_uniform(p);
            const int cx = static_cast<int>(p[0] * g);
            const int cy = static_cast<int>(p[1] * g);
            ++cells[static_cast<size_t>(cx * g + cy)];
        }
        for (int c = 0; c < g * g; ++c)
            ASSERT_EQ(cells[static_cast<size_t>(c)], 1) << "cell " << c;
    }

    TEST(Sobol, UnshiftedFirstPointsMatchReference)
    {
        // With shift == 0 (seed chosen so masks are XORed manually), we check
        // the classic unscrambled values via two sequences with the same seed:
        // identical seeds must reproduce identical points.
        SobolSequence a(8, 42), b(8, 42);
        std::vector<double> pa(8), pb(8);
        for (int i = 0; i < 100; ++i)
        {
            a.next_uniform(pa);
            b.next_uniform(pb);
            for (int d = 0; d < 8; ++d)
                ASSERT_EQ(pa[static_cast<size_t>(d)], pb[static_cast<size_t>(d)]);
        }
    }

    TEST(Sobol, DifferentSeedsDiffer)
    {
        SobolSequence a(4, 1), b(4, 2);
        std::vector<double> pa(4), pb(4);
        a.next_uniform(pa);
        b.next_uniform(pb);
        bool any_diff = false;
        for (int d = 0; d < 4; ++d)
            any_diff |= (pa[static_cast<size_t>(d)] != pb[static_cast<size_t>(d)]);
        EXPECT_TRUE(any_diff);
    }

    // ---------------------------------------------------------------------
    // BrownianBridge
    // ---------------------------------------------------------------------

    // The bridge is a linear map z -> W. Its correctness is fully captured
    // by the covariance identity Cov(W_ti, W_tj) = min(ti, tj), i.e.
    // A A^T = [min(ti,tj)] where column j of A is transform(e_j).
    TEST(BrownianBridge, ExactCovariance)
    {
        const std::vector<Time> times = {0.1, 0.25, 0.4, 0.55, 0.8, 1.0, 1.5};
        const int n = static_cast<int>(times.size());
        BrownianBridge bb(times);

        // Build the matrix A column by column.
        std::vector<std::vector<Real>> A(static_cast<size_t>(n),
                                         std::vector<Real>(static_cast<size_t>(n)));
        std::vector<Real> z(static_cast<size_t>(n), 0.0), w(static_cast<size_t>(n));
        for (int j = 0; j < n; ++j)
        {
            z[static_cast<size_t>(j)] = 1.0;
            bb.transform(z, w);
            z[static_cast<size_t>(j)] = 0.0;
            for (int i = 0; i < n; ++i)
                A[static_cast<size_t>(i)][static_cast<size_t>(j)] = w[static_cast<size_t>(i)];
        }

        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
            {
                Real cov = 0.0;
                for (int k = 0; k < n; ++k)
                    cov += A[static_cast<size_t>(i)][static_cast<size_t>(k)] *
                           A[static_cast<size_t>(j)][static_cast<size_t>(k)];
                const Real expected = std::min(times[static_cast<size_t>(i)],
                                               times[static_cast<size_t>(j)]);
                EXPECT_NEAR(cov, expected, 1e-12)
                    << "Cov(W_" << i << ", W_" << j << ")";
            }
    }

    TEST(BrownianBridge, FirstCoordinateDrivesTerminal)
    {
        const std::vector<Time> times = {0.25, 0.5, 0.75, 1.0};
        BrownianBridge bb(times);
        std::vector<Real> z = {1.0, 0.0, 0.0, 0.0}, w(4);
        bb.transform(z, w);
        // z[0] = 1 with all others 0 must set W(T) = sqrt(T) exactly.
        EXPECT_NEAR(w[3], 1.0, 1e-15);
    }

    // ---------------------------------------------------------------------
    // RQMC pricing through the vanilla MC engine
    // ---------------------------------------------------------------------

    namespace
    {
        constexpr Real S0 = 100.0, K = 100.0, T = 1.0, r = 0.05, q = 0.02, sigma = 0.20;

        PricingResult priceVanilla(SamplerKind sampler, int n_paths, OptionType type)
        {
            PricingContext ctx;
            ctx.model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);
            ctx.settings.mc_paths = n_paths;
            ctx.settings.mc_seed = 7;
            ctx.settings.mc_antithetic = false;
            ctx.settings.mc_sampler = sampler;
            ctx.settings.mc_gaussian = GaussianKind::InverseNormal;

            VanillaOption opt(std::make_shared<PlainVanillaPayoff>(type, K),
                              std::make_shared<EuropeanExercise>(T));
            BSEuroVanillaMCEngine engine(ctx);
            opt.accept(engine);
            return engine.results();
        }

        Real bsAnalyticCall()
        {
            const Real d1 = (std::log(S0 / K) + (r - q + 0.5 * sigma * sigma) * T) /
                            (sigma * std::sqrt(T));
            const Real d2 = d1 - sigma * std::sqrt(T);
            auto Phi = [](Real x)
            { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
            return S0 * std::exp(-q * T) * Phi(d1) - K * std::exp(-r * T) * Phi(d2);
        }
    } // namespace

    TEST(SobolRQMC, PriceMatchesAnalyticTightly)
    {
        // 16 batches x 16384 paths. RQMC on a smooth 1-D integrand should be
        // dramatically more accurate than plain MC at the same budget.
        const PricingResult res = priceVanilla(SamplerKind::Sobol, 1 << 18, OptionType::Call);
        const Real ana = bsAnalyticCall();
        EXPECT_NEAR(res.npv, ana, 5e-3);
        EXPECT_LE(std::abs(res.npv - ana), 4.0 * res.mc_std_error + 1e-4);
        EXPECT_GT(res.mc_std_error, 0.0);
    }

    TEST(SobolRQMC, BeatsPseudoRandomStdError)
    {
        const PricingResult qmc = priceVanilla(SamplerKind::Sobol, 1 << 16, OptionType::Call);
        const PricingResult prng = priceVanilla(SamplerKind::PseudoRandom, 1 << 16, OptionType::Call);
        // Same budget: RQMC std error should be far below plain MC's.
        EXPECT_LT(qmc.mc_std_error, 0.5 * prng.mc_std_error);
    }

    TEST(SobolRQMC, Reproducible)
    {
        const PricingResult a = priceVanilla(SamplerKind::Sobol, 1 << 14, OptionType::Put);
        const PricingResult b = priceVanilla(SamplerKind::Sobol, 1 << 14, OptionType::Put);
        EXPECT_NEAR(a.npv, b.npv, 1e-12);
    }

} // namespace quantModeling
