#include <gtest/gtest.h>

#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <functional>
#include <vector>

namespace quantModeling
{
    namespace
    {
        /// Composite Simpson rule — independent of the closed forms it checks.
        Real simpson(const std::function<Real(Real)> &f, Real a, Real b, int intervals)
        {
            const Real h = (b - a) / intervals;
            Real sum = f(a) + f(b);
            for (int k = 1; k < intervals; ++k)
                sum += (k % 2 == 1 ? 4.0 : 2.0) * f(a + k * h);
            return sum * h / 3.0;
        }

        /// E[max(V, 0)] for V ~ N(mu, sigma²) by quadrature of x × density on
        /// [0, mu + 12 sigma] (the tail beyond is below 1e-30).
        Real ee_by_quadrature(Real mu, Real sigma)
        {
            const auto integrand = [&](Real x)
            { return x * norm_pdf((x - mu) / sigma) / sigma; };
            return simpson(integrand, 0.0, std::max(mu, 0.0) + 12.0 * sigma, 20000);
        }

        Real ene_by_quadrature(Real mu, Real sigma)
        {
            const auto integrand = [&](Real x)
            { return x * norm_pdf((x - mu) / sigma) / sigma; };
            return simpson(integrand, std::min(mu, 0.0) - 12.0 * sigma, 0.0, 20000);
        }

        /// t_i = i T / n, i = 1..n.
        std::vector<Time> uniform_grid(Time T, int n)
        {
            std::vector<Time> times;
            for (int i = 1; i <= n; ++i)
                times.push_back(T * i / n);
            return times;
        }
    } // namespace

    // ── Closed forms under a normal distribution (blueprint §3.2) ───────────

    TEST(NormalExposure, MatchesNumericalIntegration)
    {
        // The acceptance criterion of lot X0: 1e-12.
        for (const Real mu : {-1.5, -0.2, 0.0, 0.3, 2.0})
            for (const Real sigma : {0.5, 1.0, 2.5})
            {
                EXPECT_NEAR(normal_expected_exposure(mu, sigma), ee_by_quadrature(mu, sigma), 1e-12)
                    << "mu=" << mu << " sigma=" << sigma;
                EXPECT_NEAR(normal_expected_negative_exposure(mu, sigma),
                            ene_by_quadrature(mu, sigma), 1e-12)
                    << "mu=" << mu << " sigma=" << sigma;
            }
    }

    TEST(NormalExposure, ExpectedExposurePlusNegativeExposureIsTheMean)
    {
        // max(V, 0) + min(V, 0) = V path by path, hence EE + ENE = EFV.
        for (const Real mu : {-3.0, -0.1, 0.0, 0.7, 4.0})
            for (const Real sigma : {0.0, 0.2, 1.0, 3.0})
            {
                const Real ee = normal_expected_exposure(mu, sigma);
                const Real ene = normal_expected_negative_exposure(mu, sigma);
                EXPECT_NEAR(ee + ene, mu, 1e-14);
                EXPECT_GE(ee, 0.0);
                EXPECT_LE(ene, 0.0);
            }
    }

    TEST(NormalExposure, GregoryRulesOfThumb)
    {
        const Real sigma = 7.0;
        // The "0.4": phi(0) = 1 / sqrt(2 pi) = 0.3989.
        EXPECT_NEAR(normal_expected_exposure(0.0, sigma), 0.3989422804014327 * sigma, 1e-14);
        EXPECT_NEAR(normal_potential_future_exposure(0.0, sigma, 0.95), 1.6448536269514722 * sigma,
                    1e-9);
        EXPECT_NEAR(normal_potential_future_exposure(0.0, sigma, 0.99), 2.3263478740408408 * sigma,
                    1e-9);
    }

    TEST(NormalExposure, PfeIsTheQuantile)
    {
        // P(V <= PFE_a) = a.
        for (const Real a : {0.5, 0.9, 0.95, 0.99, 0.999})
        {
            const Real mu = 0.4, sigma = 1.7;
            const Real pfe = normal_potential_future_exposure(mu, sigma, a);
            EXPECT_NEAR(norm_cdf((pfe - mu) / sigma), a, 1e-12);
        }
    }

    TEST(NormalExposure, DegenerateAndInvalidInputs)
    {
        EXPECT_DOUBLE_EQ(normal_expected_exposure(2.0, 0.0), 2.0);
        EXPECT_DOUBLE_EQ(normal_expected_exposure(-2.0, 0.0), 0.0);
        EXPECT_DOUBLE_EQ(normal_expected_negative_exposure(-2.0, 0.0), -2.0);
        EXPECT_THROW(normal_expected_exposure(0.0, -1.0), InvalidInput);
        EXPECT_THROW(normal_potential_future_exposure(0.0, 1.0, 1.0), InvalidInput);
        EXPECT_THROW(normal_potential_future_exposure(0.0, 1.0, 0.0), InvalidInput);
    }

    TEST(NormalExposure, ExpectedExposureIsIncreasingInMeanAndVolatility)
    {
        Real previous = normal_expected_exposure(-2.0, 1.0);
        for (Real mu = -1.9; mu <= 2.0; mu += 0.1)
        {
            const Real ee = normal_expected_exposure(mu, 1.0);
            EXPECT_GT(ee, previous);
            previous = ee;
        }
        previous = normal_expected_exposure(0.3, 0.1);
        for (Real sigma = 0.2; sigma <= 3.0; sigma += 0.1)
        {
            // Exposure is an option on the value: it has positive vega.
            const Real ee = normal_expected_exposure(0.3, sigma);
            EXPECT_GT(ee, previous);
            previous = ee;
        }
    }

    // ── Netting factor (blueprint §4.1) ──────────────────────────────────────

    TEST(NettingFactor, LimitsAndMonotonicity)
    {
        EXPECT_DOUBLE_EQ(netting_factor(1, 0.3), 1.0);
        EXPECT_NEAR(netting_factor(25, 0.0), 1.0 / 5.0, 1e-15); // independent: 1/sqrt(n)
        EXPECT_NEAR(netting_factor(25, 1.0), 1.0, 1e-15);       // directional book: no benefit
        EXPECT_NEAR(netting_factor(2, -1.0), 0.0, 1e-15);       // a perfect hedge
        Real previous = netting_factor(10, -0.1);
        for (Real rho = 0.0; rho <= 1.0; rho += 0.1)
        {
            const Real factor = netting_factor(10, rho);
            EXPECT_GT(factor, previous);
            EXPECT_LE(factor, 1.0 + 1e-15);
            previous = factor;
        }
        EXPECT_THROW(netting_factor(0, 0.0), InvalidInput);
        EXPECT_THROW(netting_factor(3, -0.6), InvalidInput); // below -1/(n-1)
        EXPECT_THROW(netting_factor(3, 1.1), InvalidInput);
    }

    TEST(NettingFactor, IsTheRatioOfNettedToGrossNormalExposure)
    {
        // n centred normals of volatility sigma and pairwise correlation rho:
        // the netted value has volatility sigma sqrt(n + n(n-1) rho), the
        // gross exposure is n times the stand-alone one.
        const std::size_t n = 12;
        const Real sigma = 3.0, rho = 0.25;
        const Real netted_sigma = sigma * std::sqrt(12.0 + 12.0 * 11.0 * rho);
        const Real netted = normal_expected_exposure(0.0, netted_sigma);
        const Real gross = 12.0 * normal_expected_exposure(0.0, sigma);
        EXPECT_NEAR(netted / gross, netting_factor(n, rho), 1e-14);
        EXPECT_LE(netted, gross); // sub-additivity of x -> max(x, 0)
    }

    // ── Functionals of a profile ─────────────────────────────────────────────

    TEST(ExposureProfileMetrics, EpeOfAForwardIsTwoThirdsOfItsFinalExposure)
    {
        // EE(t) = 0.4 sigma sqrt(t): EPE = (2/3) 0.4 sigma sqrt(T) ≈ 0.27 sigma sqrt(T).
        const Real sigma = 0.2, T = 5.0;
        const Real exact = (2.0 / 3.0) * norm_pdf(0.0) * sigma * std::sqrt(T);
        Real previous_error = 0.0;
        for (const int n : {50, 100, 200, 400, 800})
        {
            const std::vector<Time> times = uniform_grid(T, n);
            std::vector<Real> ee;
            for (const Time t : times)
                ee.push_back(norm_pdf(0.0) * sigma * std::sqrt(t));
            const Real error = std::abs(expected_positive_exposure(times, ee) - exact);
            if (previous_error > 0.0)
            {
                // The right-endpoint sum converges at order one: halving the
                // step halves the error.
                const Real order = std::log2(previous_error / error);
                EXPECT_NEAR(order, 1.0, 0.05) << "n=" << n;
            }
            previous_error = error;
        }
        EXPECT_LT(previous_error, 1e-3 * exact);
    }

    TEST(ExposureProfileMetrics, EpeOfAConstantProfileIsTheConstantOnAnyGrid)
    {
        const std::vector<Time> times = {0.1, 0.25, 1.0, 1.01, 4.0};
        EXPECT_NEAR(expected_positive_exposure(times, std::vector<Real>(5, 3.5)), 3.5, 1e-15);
        // A date at t = 0 contributes nothing.
        EXPECT_NEAR(expected_positive_exposure({0.0, 1.0, 2.0}, {99.0, 3.5, 3.5}), 3.5, 1e-15);
    }

    TEST(ExposureProfileMetrics, EffectiveExposureIsTheRunningMaximum)
    {
        // A swap-like hump: up, then down to zero.
        const std::vector<Real> ee = {1.0, 3.0, 2.5, 4.0, 2.0, 0.5, 0.0};
        const std::vector<Real> eee = effective_expected_exposure(ee);
        const std::vector<Real> expected = {1.0, 3.0, 3.0, 4.0, 4.0, 4.0, 4.0};
        ASSERT_EQ(eee.size(), ee.size());
        for (std::size_t i = 0; i < ee.size(); ++i)
        {
            EXPECT_DOUBLE_EQ(eee[i], expected[i]);
            EXPECT_GE(eee[i], ee[i]);
            if (i > 0)
            {
                EXPECT_GE(eee[i], eee[i - 1]);
            }
        }
        EXPECT_TRUE(effective_expected_exposure({}).empty());
    }

    TEST(ExposureProfileMetrics, EffectiveEpeAveragesOverTheFirstYearOnly)
    {
        const std::vector<Time> times = {0.25, 0.5, 0.75, 1.0, 2.0, 3.0};
        const std::vector<Real> ee = {2.0, 4.0, 3.0, 1.0, 10.0, 0.0};
        // Effective EE on the first year: 2, 4, 4, 4. The 10 at two years is
        // beyond the horizon.
        EXPECT_NEAR(effective_expected_positive_exposure(times, ee), 0.25 * (2.0 + 4.0 + 4.0 + 4.0),
                    1e-15);
        // Over the whole life it is the plain average of the running maximum.
        EXPECT_NEAR(effective_expected_positive_exposure(times, ee, 3.0),
                    (0.25 * 14.0 + 10.0 + 10.0) / 3.0, 1e-15);
    }

    TEST(ExposureProfileMetrics, EffectiveEpeCutsAGridThatStraddlesTheHorizon)
    {
        // Dates at 0.6 and 1.4: the second step counts for 0.4 of a year.
        EXPECT_NEAR(effective_expected_positive_exposure({0.6, 1.4}, {1.0, 5.0}),
                    0.6 * 1.0 + 0.4 * 5.0, 1e-15);
    }

    TEST(ExposureProfileMetrics, EffectiveEpeUsesTheLifeOfAShortNettingSet)
    {
        // Everything matures at six months: the average is over six months
        // (CRE53.13), not diluted over the year.
        EXPECT_NEAR(effective_expected_positive_exposure({0.25, 0.5}, {2.0, 1.0}), 2.0, 1e-15);
    }

    TEST(ExposureProfileMetrics, EffectiveEpeIsAtLeastEpeOverTheSameHorizon)
    {
        // An increasing profile is its own running maximum: equality. A
        // decreasing one is frozen at its first value: strictly above.
        const std::vector<Time> times = uniform_grid(1.0, 12);
        std::vector<Real> increasing, hump;
        for (const Time t : times)
        {
            increasing.push_back(std::sqrt(t));
            hump.push_back(std::sqrt(t) * (1.0 - t));
        }
        EXPECT_NEAR(effective_expected_positive_exposure(times, increasing),
                    expected_positive_exposure(times, increasing), 1e-15);
        EXPECT_GT(effective_expected_positive_exposure(times, hump),
                  expected_positive_exposure(times, hump));
    }

    TEST(ExposureProfileMetrics, RejectsMalformedProfiles)
    {
        EXPECT_THROW(expected_positive_exposure({}, {}), InvalidInput);
        EXPECT_THROW(expected_positive_exposure({1.0, 2.0}, {1.0}), InvalidInput);
        EXPECT_THROW(expected_positive_exposure({2.0, 1.0}, {1.0, 1.0}), InvalidInput);
        EXPECT_THROW(expected_positive_exposure({-1.0, 1.0}, {1.0, 1.0}), InvalidInput);
        EXPECT_THROW(expected_positive_exposure({0.0}, {1.0}), InvalidInput);
        EXPECT_THROW(effective_expected_positive_exposure({1.0}, {1.0}, 0.0), InvalidInput);
    }

} // namespace quantModeling
