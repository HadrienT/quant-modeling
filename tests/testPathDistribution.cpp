#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/engines/mc/path_distribution.hpp"
#include "quantModeling/engines/mc/path_simulation.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/models/equity/sabr_sim_model.hpp"
#include "quantModeling/utils/inverse_normal.hpp"

#include <cmath>
#include <numeric>

namespace quantModeling
{
    namespace
    {
        constexpr Real kSpot = 100.0, kR = 0.03, kQ = 0.01;

        DistributionSettings settings(long long n_paths, std::size_t n_steps = 24)
        {
            DistributionSettings s;
            s.n_steps = n_steps;
            s.n_paths = n_paths;
            s.n_display = 20;
            s.seed = 11;
            s.smile_dates = {n_steps / 2, n_steps};
            s.smile_moneyness = {0.8, 0.9, 1.0, 1.1, 1.2};
            return s;
        }

        /// |x - target| within z standard errors, plus a floor for bias.
        void expect_within(Real x, Real target, Real se, Real z, Real floor, const char *what)
        {
            EXPECT_NEAR(x, target, z * se + floor) << what;
        }
    } // namespace

    TEST(PathDistribution, DisplayPathsAreThoseOfSimulateModelPaths)
    {
        BlackScholesSimModel<Real> a(kSpot, kR, kQ, 0.2), b(kSpot, kR, kQ, 0.2);
        DistributionSettings s = settings(50);
        s.n_display = 50;
        const PathDistribution d = simulate_path_distribution(a, kSpot, kR, kQ, 1.0, s);
        PathSimulationSettings ps;
        ps.n_steps = s.n_steps;
        ps.n_paths = 50;
        ps.seed = s.seed;
        const SimulatedPaths p = simulate_model_paths(b, kSpot, 1.0, ps);
        ASSERT_EQ(d.paths, p.paths);
        ASSERT_EQ(d.time_grid, p.time_grid);
    }

    TEST(PathDistribution, BlackScholesSmilesAreFlatAtSigma)
    {
        const Real sigma = 0.25;
        BlackScholesSimModel<Real> m(kSpot, kR, kQ, sigma);
        DistributionSettings s = settings(40000);
        s.forward_start = s.n_steps / 2;
        const PathDistribution d = simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s);
        ASSERT_EQ(d.smiles.size(), 2u);
        for (const SmileEstimate &smile : d.smiles)
            for (std::size_t j = 0; j < smile.moneyness.size(); ++j)
                expect_within(smile.implied_vols[j], sigma, smile.iv_stderr[j], 4.0, 1e-4, "spot smile");
        ASSERT_TRUE(d.has_forward_smile);
        EXPECT_NEAR(d.forward_smile.ttm, 0.5, 1e-12);
        for (std::size_t j = 0; j < d.forward_smile.moneyness.size(); ++j)
            expect_within(d.forward_smile.implied_vols[j], sigma, d.forward_smile.iv_stderr[j], 4.0, 1e-4,
                          "forward smile");
        for (std::size_t i = 1; i < d.time_grid.size(); ++i)
            expect_within(d.atm_vol[i], sigma, d.atm_vol_stderr[i], 4.0, 1e-4, "ATM term structure");
    }

    TEST(PathDistribution, BlackScholesQuantilesAreTheLognormalOnes)
    {
        const Real sigma = 0.3, T = 2.0;
        BlackScholesSimModel<Real> m(kSpot, kR, kQ, sigma);
        const PathDistribution d = simulate_path_distribution(m, kSpot, kR, kQ, T, settings(40000));
        const std::size_t n = d.time_grid.size() - 1;
        for (std::size_t k = 0; k < d.quantile_levels.size(); ++k)
        {
            const Real z = inverse_normal_cdf(d.quantile_levels[k]);
            for (std::size_t i : {std::size_t(1), n / 2, n})
            {
                const Real t = d.time_grid[i];
                const Real exact = kSpot * std::exp((kR - kQ - 0.5 * sigma * sigma) * t + sigma * std::sqrt(t) * z);
                EXPECT_NEAR(d.spot_quantiles[k][i] / exact, 1.0, 0.015) << "q " << d.quantile_levels[k] << " t " << t;
            }
            EXPECT_DOUBLE_EQ(d.spot_quantiles[k][0], kSpot);
        }
        // Black-Scholes' instantaneous vol is sigma, on every path.
        ASSERT_TRUE(d.has_vol);
        for (std::size_t k = 0; k < d.quantile_levels.size(); ++k)
            EXPECT_DOUBLE_EQ(d.vol_quantiles[k][n], sigma);
        EXPECT_TRUE(std::isnan(d.vols[0][0]));
        EXPECT_DOUBLE_EQ(d.vols[0][n], sigma);
    }

    TEST(PathDistribution, HistogramsHoldTheCentralNinetyNinePercent)
    {
        BlackScholesSimModel<Real> m(kSpot, kR, kQ, 0.2);
        const PathDistribution d = simulate_path_distribution(m, kSpot, kR, kQ, 1.0, settings(20000));
        for (std::size_t i = 1; i < d.time_grid.size(); ++i)
        {
            const long long inside = std::accumulate(d.hist_counts[i].begin(), d.hist_counts[i].end(), 0LL);
            EXPECT_GE(inside, 19780) << i; // the 0.5% to 99.5% quantiles, both ends included
            EXPECT_LE(inside, 20000) << i;
            EXPECT_LT(d.hist_lo[i], d.hist_hi[i]);
        }
    }

    TEST(PathDistribution, DeflatedSpotIsAMartingaleUnderEveryModel)
    {
        const std::vector<Real> K{50.0, 100.0, 200.0}, Tg{0.1, 2.0};
        // A local vol with a skew: 30% at 50, 20% at 100, 15% at 200.
        const std::vector<Real> sig{0.30, 0.30, 0.20, 0.20, 0.15, 0.15};
        LocalVolSimModel<Real> lv(kSpot, kR, kQ, K, Tg, sig, 1.0 / 100.0);
        BatesSimModel<Real> heston(kSpot, kR, kQ, 0.04, 2.0, 0.04, 0.5, -0.7, 0.0, 0.0, 0.0, 1.0 / 100.0);
        for (ISimulationModel<Real> *m : std::vector<ISimulationModel<Real> *>{&lv, &heston})
        {
            const PathDistribution d = simulate_path_distribution(*m, kSpot, kR, kQ, 1.0, settings(20000));
            for (std::size_t i = 1; i < d.time_grid.size(); ++i)
                expect_within(d.deflated_mean[i], d.deflated_exact[i], d.deflated_stderr[i], 4.0, 1e-9, "martingale");
        }
    }

    TEST(PathDistribution, HestonSmileIsTheCosOneWithItsSkew)
    {
        const HestonParams h{0.04, 2.0, 0.04, 0.5, -0.7};
        BatesSimModel<Real> m(kSpot, kR, kQ, h.v0, h.kappa, h.theta, h.xi, h.rho, 0.0, 0.0, 0.0, 1.0 / 252.0);
        const PathDistribution d = simulate_path_distribution(m, kSpot, kR, kQ, 1.0, settings(40000));
        const SmileEstimate &smile = d.smiles.back();
        const Real df = std::exp(-kR * smile.ttm);
        for (std::size_t j = 0; j < smile.strikes.size(); ++j)
        {
            const Real call = heston_cos_price(smile.forward, smile.strikes[j], smile.ttm, df, h, true);
            const Real iv = black76_implied_vol(call, smile.forward, smile.strikes[j], smile.ttm, df);
            // Full-truncation Euler on a daily grid: a few tenths of a vol point of bias.
            expect_within(smile.implied_vols[j], iv, smile.iv_stderr[j], 4.0, 0.003, "Heston smile");
        }
        EXPECT_GT(smile.implied_vols.front(), smile.implied_vols.back() + 0.02); // rho < 0: a downward skew
        // The instantaneous vol starts at sqrt(v0) and spreads out.
        ASSERT_TRUE(d.has_vol);
        const std::size_t n = d.time_grid.size() - 1;
        EXPECT_NEAR(d.vol_quantiles[2][1], std::sqrt(h.v0), 0.01);
        EXPECT_GT(d.vol_quantiles[4][n] - d.vol_quantiles[0][n], d.vol_quantiles[4][1] - d.vol_quantiles[0][1]);
    }

    TEST(PathDistribution, SabrWithBetaOneAndNoVolOfVolIsBlackScholes)
    {
        const Real alpha = 0.2;
        SABRSimModel<Real> m(kSpot, kR, alpha, 1.0, 0.0, 0.0, 1.0 / 100.0);
        // A forward: no drift, so q = r.
        const PathDistribution d = simulate_path_distribution(m, kSpot, kR, kR, 1.0, settings(40000));
        for (const SmileEstimate &smile : d.smiles)
        {
            EXPECT_DOUBLE_EQ(smile.forward, kSpot);
            for (std::size_t j = 0; j < smile.moneyness.size(); ++j)
                // Euler on F (not on ln F): a small bias, of order sigma^2 dt.
                expect_within(smile.implied_vols[j], alpha, smile.iv_stderr[j], 4.0, 0.002, "SABR smile");
        }
        ASSERT_TRUE(d.has_vol);
        EXPECT_NEAR(d.vol_quantiles[0][1], alpha, 1e-12);
        EXPECT_NEAR(d.vol_quantiles[4][d.time_grid.size() - 1], alpha, 1e-12);
    }

    TEST(PathDistribution, UpAndOutCallIsBelowTheCallAndReachesItForAFarBarrier)
    {
        const Real sigma = 0.2;
        BlackScholesSimModel<Real> m1(kSpot, kR, kQ, sigma), m2(kSpot, kR, kQ, sigma);
        DistributionSettings s = settings(40000);
        s.barrier = 1.2;
        s.barrier_strike = 1.0;
        const PathDistribution near = simulate_path_distribution(m1, kSpot, kR, kQ, 1.0, s);
        ASSERT_TRUE(near.has_barrier);
        EXPECT_LT(near.barrier_price, 0.5 * near.vanilla_price);
        EXPECT_GT(near.barrier_price, 0.0);
        // The vanilla on the same paths is Black's price.
        const Real F = kSpot * std::exp((kR - kQ)), df = std::exp(-kR);
        expect_within(near.vanilla_price, black76_call_price(F, kSpot, 1.0, sigma, df), near.vanilla_stderr, 4.0, 0.0,
                      "vanilla");
        s.barrier = 100.0;
        const PathDistribution far = simulate_path_distribution(m2, kSpot, kR, kQ, 1.0, s);
        EXPECT_DOUBLE_EQ(far.barrier_price, far.vanilla_price);
        EXPECT_DOUBLE_EQ(far.vanilla_price, near.vanilla_price); // same paths
    }

    TEST(PathDistribution, RejectsInconsistentSettings)
    {
        BlackScholesSimModel<Real> m(kSpot, kR, kQ, 0.2);
        DistributionSettings s = settings(100);
        s.smile_dates = {0};
        EXPECT_THROW(simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s), InvalidInput);
        s = settings(100);
        s.forward_start = s.n_steps;
        EXPECT_THROW(simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s), InvalidInput);
        s = settings(100);
        s.barrier = 0.9;
        s.barrier_strike = 0.8;
        EXPECT_THROW(simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s), InvalidInput);
        s = settings(100);
        s.n_display = 101;
        EXPECT_THROW(simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s), InvalidInput);
        s = settings(100);
        s.quantiles = {0.0};
        EXPECT_THROW(simulate_path_distribution(m, kSpot, kR, kQ, 1.0, s), InvalidInput);
    }

} // namespace quantModeling
