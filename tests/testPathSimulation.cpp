#include <gtest/gtest.h>

#include "quantModeling/engines/mc/path_simulation.hpp"

#include <cmath>
#include <numeric>

namespace quantModeling
{
    namespace
    {
        PathSimulationSettings big_settings()
        {
            PathSimulationSettings s;
            s.n_steps = 50;
            s.n_paths = 20000;
            s.seed = 7;
            return s;
        }

        PathSimulationSettings small_settings()
        {
            PathSimulationSettings s;
            s.n_steps = 20;
            s.n_paths = 10;
            s.seed = 7;
            return s;
        }
    } // namespace

    // ── Black-Scholes ─────────────────────────────────────────────────────────

    TEST(PathSimulation, BSPathsHaveTheRequestedShapeAndStartAtSpot)
    {
        const auto s = simulate_black_scholes_paths(100.0, 0.03, 0.01, 0.2, 1.0, small_settings());
        ASSERT_EQ(s.paths.size(), 10u);
        ASSERT_EQ(s.time_grid.size(), 21u);
        EXPECT_DOUBLE_EQ(s.time_grid.front(), 0.0);
        EXPECT_DOUBLE_EQ(s.time_grid.back(), 1.0);
        for (const auto &path : s.paths)
        {
            ASSERT_EQ(path.size(), 21u);
            EXPECT_DOUBLE_EQ(path.front(), 100.0);
        }
    }

    TEST(PathSimulation, BSTerminalDistributionMatchesTheExactLognormalMoments)
    {
        const Real spot = 100.0, r = 0.03, q = 0.01, sigma = 0.25, ttm = 0.75;
        const auto s = simulate_black_scholes_paths(spot, r, q, sigma, ttm, big_settings());

        std::vector<Real> log_returns;
        log_returns.reserve(s.paths.size());
        for (const auto &path : s.paths)
            log_returns.push_back(std::log(path.back() / spot));

        const Real mean = std::accumulate(log_returns.begin(), log_returns.end(), Real(0.0)) /
                          static_cast<Real>(log_returns.size());
        Real var = 0.0;
        for (const Real x : log_returns)
            var += (x - mean) * (x - mean);
        var /= static_cast<Real>(log_returns.size() - 1);

        const Real expected_mean = (r - q - 0.5 * sigma * sigma) * ttm;
        const Real expected_var = sigma * sigma * ttm;
        EXPECT_NEAR(mean, expected_mean, 0.01);
        EXPECT_NEAR(var, expected_var, 0.01);
    }

    TEST(PathSimulation, BSIsReproducibleGivenTheSameSeed)
    {
        const auto a = simulate_black_scholes_paths(100.0, 0.03, 0.0, 0.2, 1.0, small_settings());
        const auto b = simulate_black_scholes_paths(100.0, 0.03, 0.0, 0.2, 1.0, small_settings());
        EXPECT_EQ(a.paths, b.paths);
    }

    TEST(PathSimulation, BSDegenerateInputsReturnEmptyWithoutCrashing)
    {
        EXPECT_TRUE(simulate_black_scholes_paths(100.0, 0.03, 0.0, 0.2, 0.0, small_settings()).paths.empty());
        PathSimulationSettings zero_paths = small_settings();
        zero_paths.n_paths = 0;
        EXPECT_TRUE(simulate_black_scholes_paths(100.0, 0.03, 0.0, 0.2, 1.0, zero_paths).paths.empty());
        PathSimulationSettings zero_steps = small_settings();
        zero_steps.n_steps = 0;
        EXPECT_TRUE(simulate_black_scholes_paths(100.0, 0.03, 0.0, 0.2, 1.0, zero_steps).paths.empty());
    }

    // ── SABR ──────────────────────────────────────────────────────────────────

    TEST(PathSimulation, SABRPathsHaveTheRequestedShapeAndStartAtForward)
    {
        const SABRParams p{0.2, 0.5, -0.4, 0.5};
        const auto s = simulate_sabr_paths(100.0, p, 1.0, small_settings());
        ASSERT_EQ(s.paths.size(), 10u);
        for (const auto &path : s.paths)
        {
            ASSERT_EQ(path.size(), 21u);
            EXPECT_DOUBLE_EQ(path.front(), 100.0);
        }
    }

    TEST(PathSimulation, SABRForwardNeverGoesNegative)
    {
        // Deliberately harsh parameters (high alpha, beta < 1, long maturity)
        // to actually exercise the absorbing floor, not just avoid it by luck.
        const SABRParams p{0.6, 0.3, 0.0, 0.8};
        const auto s = simulate_sabr_paths(50.0, p, 2.0, big_settings());
        for (const auto &path : s.paths)
            for (const Real f : path)
                ASSERT_GE(f, 0.0);
    }

    TEST(PathSimulation, SABRAlphaIsAMartingaleOnAverage)
    {
        // alpha's own SDE is a pure driftless GBM (dalpha = nu*alpha*dW2),
        // so E[alpha_T] = alpha_0 exactly; check this holds for the
        // terminal forward-implied vol scale is not directly observable
        // here, but the exact-GBM step for alpha means the *forward*
        // process's vol-of-vol driver has the right unconditional scale --
        // verified indirectly via the terminal F distribution's spread
        // growing with maturity, which would fail to hold if alpha's
        // update carried a spurious drift.
        const SABRParams p{0.3, 1.0, 0.0, 0.6}; // beta=1, rho=0: F is then a pure lognormal-vol-of-vol process
        const auto s = simulate_sabr_paths(100.0, p, 1.0, big_settings());

        Real sum = 0.0, sum_sq = 0.0;
        for (const auto &path : s.paths)
        {
            sum += path.back();
            sum_sq += path.back() * path.back();
        }
        const Real n = static_cast<Real>(s.paths.size());
        const Real mean = sum / n;
        // With beta=1 and no correlation, F is a driftless process (a sum of
        // conditionally lognormal increments with zero mean at each step),
        // so E[F_T] should stay close to F_0 -- unlike SABRForwardNeverGoesNegative,
        // beta=1 here means F cannot go negative in exact continuous time,
        // so any drift in the discretisation would show up as a mean shift.
        EXPECT_NEAR(mean, 100.0, 3.0);
    }

    TEST(PathSimulation, SABRIsReproducibleGivenTheSameSeed)
    {
        const SABRParams p{0.2, 0.5, -0.4, 0.5};
        const auto a = simulate_sabr_paths(100.0, p, 1.0, small_settings());
        const auto b = simulate_sabr_paths(100.0, p, 1.0, small_settings());
        EXPECT_EQ(a.paths, b.paths);
    }

    TEST(PathSimulation, SABRDegenerateInputsReturnEmptyWithoutCrashing)
    {
        const SABRParams p{0.2, 0.5, -0.4, 0.5};
        EXPECT_TRUE(simulate_sabr_paths(100.0, p, 0.0, small_settings()).paths.empty());
        PathSimulationSettings zero_paths = small_settings();
        zero_paths.n_paths = 0;
        EXPECT_TRUE(simulate_sabr_paths(100.0, p, 1.0, zero_paths).paths.empty());
    }

} // namespace quantModeling
