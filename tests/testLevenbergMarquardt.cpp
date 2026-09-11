#include <gtest/gtest.h>

#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <random>
#include <vector>

namespace quantModeling::calibration
{
    namespace
    {

        Real bs_call_price(Real S, Real K, Real T, Real r, Real q, Real sigma)
        {
            const Real vol_sqrt_t = sigma * std::sqrt(T);
            const Real d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / vol_sqrt_t;
            const Real d2 = d1 - vol_sqrt_t;
            return S * std::exp(-q * T) * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d2);
        }

        // ── Toy problem 1: linear least squares y = a*x + b ────────────────
        // Has a known closed-form-adjacent solution; exercises the multi-
        // parameter path with no bounds.
        struct LinearFit final : public ObjectiveFunction
        {
            std::vector<Real> x, y;

            std::size_t num_params() const override { return 2; }
            std::size_t num_residuals() const override { return x.size(); }

            std::vector<Real> residuals(const std::vector<Real> &p) const override
            {
                std::vector<Real> r(x.size());
                for (std::size_t i = 0; i < x.size(); ++i)
                    r[i] = (p[0] * x[i] + p[1]) - y[i];
                return r;
            }
        };

        // ── Toy problem 2: single-parameter Black-Scholes vol calibration ──
        // This is exactly the shape of a real smile calibration (one strike
        // slice, price residuals), just with one parameter instead of SVI's
        // five.
        struct BSVolFit final : public ObjectiveFunction
        {
            Real S = 0, T = 0, r = 0, q = 0;
            std::vector<Real> strikes;
            std::vector<Real> market_prices;

            std::size_t num_params() const override { return 1; }
            std::size_t num_residuals() const override { return strikes.size(); }

            std::vector<Real> residuals(const std::vector<Real> &p) const override
            {
                std::vector<Real> res(strikes.size());
                for (std::size_t i = 0; i < strikes.size(); ++i)
                    res[i] = bs_call_price(S, strikes[i], T, r, q, p[0]) - market_prices[i];
                return res;
            }

            std::vector<Real> lower_bounds() const override { return {1e-4}; }
            std::vector<Real> upper_bounds() const override { return {5.0}; }
        };

    } // namespace

    TEST(LevenbergMarquardt, RecoversLinearFitParameters)
    {
        LinearFit fit;
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 0.01);
        constexpr Real true_a = 2.5, true_b = -1.0;
        for (int i = 0; i < 50; ++i)
        {
            const Real xi = static_cast<Real>(i) * 0.1;
            fit.x.push_back(xi);
            fit.y.push_back(true_a * xi + true_b + noise(rng));
        }

        const auto report = levenberg_marquardt(fit, {0.0, 0.0});

        EXPECT_TRUE(report.converged);
        ASSERT_EQ(report.params.size(), 2u);
        EXPECT_NEAR(report.params[0], true_a, 0.05);
        EXPECT_NEAR(report.params[1], true_b, 0.05);
    }

    TEST(LevenbergMarquardt, RecoversBlackScholesVolatilityFromSyntheticPrices)
    {
        BSVolFit fit;
        fit.S = 100.0;
        fit.T = 1.0;
        fit.r = 0.03;
        fit.q = 0.01;
        constexpr Real true_sigma = 0.22;
        fit.strikes = {70, 80, 90, 100, 110, 120, 130};
        for (const Real K : fit.strikes)
            fit.market_prices.push_back(bs_call_price(fit.S, K, fit.T, fit.r, fit.q, true_sigma));

        const auto report = levenberg_marquardt(fit, {0.10});

        EXPECT_TRUE(report.converged);
        ASSERT_EQ(report.params.size(), 1u);
        EXPECT_NEAR(report.params[0], true_sigma, 1e-6);
        EXPECT_LT(report.rmse, 1e-8);
    }

    TEST(LevenbergMarquardt, ClampsToBoundsWhenUnconstrainedOptimumIsOutside)
    {
        BSVolFit fit;
        fit.S = 100.0;
        fit.T = 1.0;
        fit.r = 0.0;
        fit.q = 0.0;
        fit.strikes = {90, 100, 110};
        // Prices consistent with an unreachable sigma=8; the bound caps at 5.
        for (const Real K : fit.strikes)
            fit.market_prices.push_back(bs_call_price(fit.S, K, fit.T, fit.r, fit.q, 8.0));

        const auto report = levenberg_marquardt(fit, {1.0});

        ASSERT_EQ(report.params.size(), 1u);
        EXPECT_LE(report.params[0], 5.0 + 1e-9);
        EXPECT_GE(report.params[0], 1e-4 - 1e-9);
    }

    TEST(LevenbergMarquardt, ReducesCostFromABadInitialGuess)
    {
        BSVolFit fit;
        fit.S = 100.0;
        fit.T = 0.5;
        fit.r = 0.02;
        fit.q = 0.0;
        fit.strikes = {80, 100, 120};
        constexpr Real true_sigma = 0.35;
        for (const Real K : fit.strikes)
            fit.market_prices.push_back(bs_call_price(fit.S, K, fit.T, fit.r, fit.q, true_sigma));

        const std::vector<Real> initial_params = {0.05};
        Real initial_cost = 0.0;
        for (const Real ri : fit.residuals(initial_params))
            initial_cost += ri * ri;

        const auto report = levenberg_marquardt(fit, initial_params);

        Real final_cost = 0.0;
        for (const Real ri : fit.residuals(report.params))
            final_cost += ri * ri;

        EXPECT_LT(final_cost, initial_cost);
        EXPECT_TRUE(report.converged);
    }

    TEST(LevenbergMarquardt, IsDeterministicGivenTheSameInputs)
    {
        BSVolFit fit;
        fit.S = 100.0;
        fit.T = 1.0;
        fit.r = 0.01;
        fit.q = 0.0;
        fit.strikes = {85, 95, 105, 115};
        for (const Real K : fit.strikes)
            fit.market_prices.push_back(bs_call_price(fit.S, K, fit.T, fit.r, fit.q, 0.28));

        const auto r1 = levenberg_marquardt(fit, {0.15});
        const auto r2 = levenberg_marquardt(fit, {0.15});

        ASSERT_EQ(r1.params.size(), r2.params.size());
        EXPECT_DOUBLE_EQ(r1.params[0], r2.params[0]);
        EXPECT_EQ(r1.iterations, r2.iterations);
    }

} // namespace quantModeling::calibration
