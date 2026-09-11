#include <gtest/gtest.h>

#include "quantModeling/market/svi.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        /// A standard, textbook-reasonable set of SVI parameters (negative
        /// skew typical of equity smiles), used as the "nice" fixture across
        /// several tests.
        SVIParams nice_params()
        {
            SVIParams p;
            p.a = 0.04;
            p.b = 0.10;
            p.rho = -0.40;
            p.m = 0.0;
            p.sigma = 0.30;
            return p;
        }
    } // namespace

    TEST(SVI, TotalVarianceAtTheTranslationPointIsAPlusBSigma)
    {
        const SVIParams p = nice_params();
        EXPECT_NEAR(svi_total_variance(p.m, p), p.a + p.b * p.sigma, 1e-12);
    }

    TEST(SVI, FirstDerivativeMatchesCentralFiniteDifference)
    {
        const SVIParams p = nice_params();
        const Real h = 1e-6;
        for (Real k = -1.0; k <= 1.0; k += 0.1)
        {
            const Real analytic = svi_total_variance_dk(k, p);
            const Real fd = (svi_total_variance(k + h, p) - svi_total_variance(k - h, p)) / (2.0 * h);
            EXPECT_NEAR(analytic, fd, 1e-6);
        }
    }

    TEST(SVI, SecondDerivativeMatchesCentralFiniteDifference)
    {
        const SVIParams p = nice_params();
        const Real h = 1e-4;
        for (Real k = -1.0; k <= 1.0; k += 0.1)
        {
            const Real analytic = svi_total_variance_dk2(k, p);
            const Real fd = (svi_total_variance_dk(k + h, p) - svi_total_variance_dk(k - h, p)) / (2.0 * h);
            EXPECT_NEAR(analytic, fd, 1e-4);
        }
    }

    TEST(SVI, TotalVarianceIsConvexInLogMoneynessWhenBIsPositive)
    {
        // w''(k) = b*sigma^2/D^3 > 0 whenever b > 0 -- an algebraic fact of
        // the raw parametrisation, independent of a/rho/m.
        const SVIParams p = nice_params();
        for (Real k = -2.0; k <= 2.0; k += 0.25)
            EXPECT_GT(svi_total_variance_dk2(k, p), 0.0);
    }

    TEST(SVI, ImpliedVolIsTheSquareRootOfTotalVarianceOverT)
    {
        const SVIParams p = nice_params();
        const Real T = 0.75;
        const Real k = 0.1;
        const Real expected = std::sqrt(svi_total_variance(k, p) / T);
        EXPECT_NEAR(svi_implied_vol(k, T, p), expected, 1e-12);
    }

    TEST(SVI, ImpliedVolIsFlooredAtZeroForADegenerateNegativeVarianceSlice)
    {
        SVIParams p;
        p.a = -10.0; // pushes total variance negative everywhere nearby
        p.b = 0.01;
        p.rho = 0.0;
        p.m = 0.0;
        p.sigma = 0.1;
        EXPECT_DOUBLE_EQ(svi_implied_vol(0.0, 1.0, p), 0.0);
    }

    // ── Necessary conditions ────────────────────────────────────────────────

    TEST(SVI, NecessaryConditionsAcceptANiceParameterSet)
    {
        EXPECT_TRUE(svi_satisfies_necessary_conditions(nice_params()));
    }

    TEST(SVI, NecessaryConditionsRejectNegativeB)
    {
        SVIParams p = nice_params();
        p.b = -0.1;
        EXPECT_FALSE(svi_satisfies_necessary_conditions(p));
    }

    TEST(SVI, NecessaryConditionsRejectRhoOutOfRange)
    {
        SVIParams p = nice_params();
        p.rho = 1.0;
        EXPECT_FALSE(svi_satisfies_necessary_conditions(p));
        p.rho = -1.2;
        EXPECT_FALSE(svi_satisfies_necessary_conditions(p));
    }

    TEST(SVI, NecessaryConditionsRejectNonPositiveSigma)
    {
        SVIParams p = nice_params();
        p.sigma = 0.0;
        EXPECT_FALSE(svi_satisfies_necessary_conditions(p));
    }

    TEST(SVI, NecessaryConditionsRejectANegativeVarianceMinimum)
    {
        SVIParams p = nice_params();
        p.a = -1.0; // a + b*sigma*sqrt(1-rho^2) < 0 for these b, rho, sigma
        EXPECT_FALSE(svi_satisfies_necessary_conditions(p));
    }

    // ── Butterfly (density) arbitrage ──────────────────────────────────────

    TEST(SVI, ButterflyCheckAcceptsANiceParameterSetOnAWideRange)
    {
        EXPECT_TRUE(svi_is_butterfly_arbitrage_free(nice_params(), -3.0, 3.0));
    }

    TEST(SVI, ButterflyCheckRejectsAnExtremeWingSteepnessRelativeToCurvature)
    {
        // A very steep wing (large b) against a tiny curvature (sigma) is the
        // textbook way to force the density negative near the wings.
        SVIParams p;
        p.a = 0.01;
        p.b = 5.0;
        p.rho = 0.9;
        p.m = 0.0;
        p.sigma = 0.01;
        EXPECT_FALSE(svi_is_butterfly_arbitrage_free(p, -3.0, 3.0));
    }

    TEST(SVI, ButterflyCheckRejectsWhenNecessaryConditionsAlreadyFail)
    {
        SVIParams p = nice_params();
        p.b = -1.0;
        EXPECT_FALSE(svi_is_butterfly_arbitrage_free(p, -1.0, 1.0));
    }

} // namespace quantModeling
