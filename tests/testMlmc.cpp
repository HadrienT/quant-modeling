#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/engines/mc/approx_inverse_cdf.hpp"
#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/engines/mc/mlqmc.hpp"
#include "quantModeling/utils/thread_pool.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/sabr_sim_model.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Multilevel Monte Carlo over the time step (blueprint/wp/26-performance.md
// §8, WP 26 P4a).

namespace quantModeling
{
    namespace
    {
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;

        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2026-01-01")};
        }

        // A one-year call (put), strike 95.
        const std::string kCall = "2027-01-01\n    pays max(spot() - 95, 0)\n";
        const std::string kPut = "2027-01-01\n    pays max(95 - spot(), 0)\n";

        // Coarse level 0: monthly log-Euler steps.
        ScriptModelSpec local_vol(bool flat)
        {
            ScriptModelSpec s;
            s.model = "local_vol";
            s.spot = 100.0;
            s.rate = 0.05;
            s.dividend = 0.02;
            s.K_grid = {50.0, 80.0, 100.0, 120.0, 200.0};
            s.T_grid = {0.0, 0.5, 2.0};
            if (flat)
                s.sigma_loc_flat.assign(15, 0.25);
            else
                s.sigma_loc_flat = {0.80, 0.70, 0.60, 0.45, 0.40, 0.35, 0.25, 0.24, 0.22,
                                    0.16, 0.15, 0.14, 0.10, 0.10, 0.10};
            s.max_dt = 1.0 / 12.0;
            return s;
        }

        /// Plain Monte Carlo on the grid of level `level`.
        SimulationMCResult plain(const std::string &script, const ScriptModelSpec &spec, int level, int paths)
        {
            ScriptedProduct<Real> product(script, ctx());
            auto m = make_script_model<Real>(spec);
            EXPECT_TRUE(m->set_refinement(1 << level));
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 5;
            s.mc_sampler = SamplerKind::PseudoRandom;
            s.mc_control_variate = false;
            return simulate<Real>(product, *m, s);
        }
    } // namespace

    TEST(Mlmc, RefinedGridsNest)
    {
        const TimeLine base = add_monitoring_steps({0.3, 1.0}, 0.25); // uneven steps
        const TimeLine coarse = refine_steps(base, 4), fine = refine_steps(base, 8);
        EXPECT_EQ(refine_steps(base, 1), base);
        ASSERT_EQ(fine.size(), 2 * coarse.size());
        for (std::size_t k = 0; k < coarse.size(); ++k)
            EXPECT_EQ(fine[2 * k + 1], coarse[k]) << k; // exactly, not within a tolerance
        EXPECT_THROW(refine_steps(base, 0), InvalidInput);
    }

    TEST(Mlmc, EventsFallOnTheClosestStepOfAFineGrid)
    {
        // A daily grid split 64 ways: steps of 22 minutes, shorter than
        // TIMELINE_EPS (an hour). The event must be the step on it, not the
        // first one within the tolerance.
        const TimeLine sim = refine_steps(add_monitoring_steps({10.0 / 365.0}, 1.0 / 365.0), 64);
        const std::vector<std::size_t> idx = event_indices(sim, {10.0 / 365.0});
        ASSERT_EQ(idx.size(), 1u);
        EXPECT_EQ(idx[0], sim.size() - 1);
    }

    TEST(Mlmc, LocalVolRefinesItsGrid)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        m->init(product.timeline(), product.defline());
        const std::size_t base = m->sim_dim();
        auto r = m->clone();
        ASSERT_TRUE(r->set_refinement(8));
        r->init(product.timeline(), product.defline());
        EXPECT_EQ(r->sim_dim(), 8 * base);
        EXPECT_FALSE(r->set_refinement(0));
    }

    TEST(Mlmc, AModelWithoutTimeStepBiasIsRefused)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        auto m = make_script_model<Real>(bs);
        EXPECT_THROW(simulate_mlmc_fixed(product, *m, {100, 100}, 1), InvalidInput);
    }

    TEST(Mlmc, AFlatSurfaceHasNothingToCorrect)
    {
        // Log-Euler is exact for a constant volatility: the fine and the
        // coarse terminal spots are the same sum of the same increments, so
        // every correction level is zero up to rounding -- the coupling's
        // sharpest check.
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(true));
        const MlmcResult r = simulate_mlmc_fixed(product, *m, {2000, 2000, 2000, 2000}, 3);
        for (int l = 1; l <= 3; ++l)
        {
            EXPECT_LT(std::fabs(r.levels[l].mean()), 1e-12) << l;
            EXPECT_LT(r.levels[l].variance(), 1e-20) << l;
        }
    }

    TEST(Mlmc, TheTelescopingSumIsPlainMonteCarloOnTheFinestGrid)
    {
        // Fixed levels 0..3: an unbiased estimate of E[P_3], the price on
        // the grid of level 3, which plain Monte Carlo estimates directly.
        ScriptedProduct<Real> product(kPut, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        const MlmcResult ml = simulate_mlmc_fixed(product, *m, {200000, 40000, 20000, 10000}, 7);
        const SimulationMCResult pl = plain(kPut, local_vol(false), 3, 200000);
        EXPECT_NEAR(ml.value, pl.npv(), 4.0 * std::hypot(ml.std_error, pl.std_error()))
            << ml.diagnostics;
    }

    TEST(Mlmc, LocalVolEulerRatesAreMeasured)
    {
        // Log-Euler under a skewed local vol: strong order 1/2 (beta ~ 1),
        // cost doubling per level (gamma ~ 1), by regression over levels
        // 1..5 as the algorithm does. Its weak error is too small here (about
        // 0.009 on level 1) to measure alpha without ~1e8 paths: the Euler-GBM
        // test bed below measures it.
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        const MlmcResult r = simulate_mlmc_fixed(product, *m, {1000, 400000, 200000, 100000, 50000, 25000}, 11);
        EXPECT_NEAR(r.beta, 1.0, 0.25) << r.diagnostics;
        EXPECT_NEAR(r.gamma, 1.0, 0.1) << r.diagnostics;
    }

    namespace
    {
        /**
         * The test bed of WP 26 §8.6: GBM stepped by Euler on S itself,
         * S <- S (1 + r h + sigma sqrt(h) z), whose weak error is known:
         * E[S_T] = S0 (1 + r h)^n against S0 e^{rT}, an O(h) bias
         * (alpha = 1), and whose strong error gives beta = 1 -- beta = 2 with
         * the Milstein term ½ sigma² h (z² - 1) (Milstein's weak bias is the
         * same: E[z² - 1] = 0).
         */
        class EulerGbm final : public ISimulationModel<Real>
        {
          public:
            EulerGbm(Real s0, Real r, Real sigma, Time max_dt, bool milstein = false)
                : s0_(s0), r_(r), sigma_(sigma), max_dt_(max_dt), milstein_(milstein)
            {
            }
            std::size_t n_underlyings() const override { return 1; }
            bool set_refinement(int r) override { return r >= 1 && (refinement_ = r); }
            void init(const TimeLine &product_timeline, const std::vector<SampleDef> &defline) override
            {
                const TimeLine events = canonical_timeline(product_timeline);
                sim_ = refine_steps(add_monitoring_steps(events, max_dt_), refinement_);
                event_of_step_ = event_index_of_step(sim_, events);
                defline_ = defline;
            }
            const TimeLine &sim_timeline() const override { return sim_; }
            std::size_t sim_dim() const override { return sim_.size(); }
            BrownianLayout brownian_layout() const override { return BrownianLayout{sim_, 1, 1}; }
            void generate_path(std::span<const double> z, Scenario<Real> &path) const override
            {
                Real S = s0_;
                Time prev = 0.0;
                for (std::size_t k = 0; k < sim_.size(); ++k)
                {
                    const Time h = sim_[k] - prev;
                    S *= 1.0 + r_ * h + sigma_ * std::sqrt(h) * z[k] +
                         (milstein_ ? 0.5 * sigma_ * sigma_ * h * (z[k] * z[k] - 1.0) : 0.0);
                    prev = sim_[k];
                    if (event_of_step_[k] < 0)
                        continue;
                    const auto e = static_cast<std::size_t>(event_of_step_[k]);
                    path[e].spots.assign(1, S);
                    path[e].numeraire = std::exp(r_ * sim_[k]);
                    path[e].discounts.assign(defline_[e].discount_mats.size(), 1.0);
                    path[e].forwards.assign(defline_[e].forward_mats.size(), S);
                }
            }
            std::unique_ptr<ISimulationModel<Real>> clone() const override
            {
                return std::make_unique<EulerGbm>(*this);
            }
            const std::vector<Real *> &parameters() const override { return params_; }
            const std::vector<std::string> &parameter_labels() const override { return labels_; }

          private:
            Real s0_, r_, sigma_;
            Time max_dt_;
            bool milstein_;
            int refinement_ = 1;
            TimeLine sim_;
            std::vector<std::ptrdiff_t> event_of_step_;
            std::vector<SampleDef> defline_;
            std::vector<Real *> params_;
            std::vector<std::string> labels_;
        };

        // A one-year forward: the deflated spot, whose exact price is S0.
        const std::string kSpot = "2027-01-01\n    pays spot()\n";
    } // namespace

    TEST(Mlmc, EulerGbmRatesAreMeasured)
    {
        // r = 50 % makes the bias large: level 0 (monthly steps) is about
        // S0 (e^{rT} - (1 + r/12)^12) e^{-rT} ~ 1.0 off.
        ScriptedProduct<Real> product(kSpot, ctx());
        const EulerGbm m(100.0, 0.5, 0.2, 1.0 / 12.0);
        const MlmcResult r = simulate_mlmc_fixed(product, m, {1000, 200000, 100000, 50000, 25000, 12500}, 19);
        EXPECT_NEAR(r.alpha, 1.0, 0.15) << r.diagnostics;
        EXPECT_NEAR(r.beta, 1.0, 0.25) << r.diagnostics;

        // Each level's mean is the analytic difference of the biases.
        const Real T = 365.0 / 365.0; // the script's year, ACT/365
        auto euler = [&](int level)
        {
            const int n = 12 << level;
            return 100.0 * std::pow(1.0 + 0.5 * T / n, n) * std::exp(-0.5 * T);
        };
        for (int l = 1; l <= 5; ++l)
            EXPECT_NEAR(r.levels[l].mean(), euler(l) - euler(l - 1),
                        4.0 * std::sqrt(r.levels[l].variance() / static_cast<double>(r.levels[l].n)))
                << l;
    }

    TEST(Mlmc, TheAdaptiveRunPricesTheEulerGbmForward)
    {
        // The exact price is S0: the adaptive run must land within its
        // target, its bias removed by the levels it chose.
        ScriptedProduct<Real> product(kSpot, ctx());
        const EulerGbm m(100.0, 0.5, 0.2, 1.0 / 12.0);
        MlmcSettings s;
        s.eps = 0.02;
        s.seed = 23;
        const MlmcResult r = simulate_mlmc(product, m, s);
        EXPECT_TRUE(r.bias_converged) << r.diagnostics;
        EXPECT_GE(r.finest_level(), 4) << r.diagnostics; // ~1.0 at level 0 halves per level
        EXPECT_NEAR(r.value, 100.0, 3.0 * s.eps) << r.diagnostics;
    }

    TEST(Mlmc, TheAdaptiveRunReachesItsTarget)
    {
        // The variance gets half the mean-square budget, the bias test the
        // other half; the price is the one a much finer grid gives.
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        MlmcSettings s;
        s.eps = 0.01;
        s.seed = 13;
        const MlmcResult r = simulate_mlmc(product, *m, s);
        EXPECT_TRUE(r.bias_converged) << r.diagnostics;
        EXPECT_LE(r.std_error, 1.02 * s.eps / std::sqrt(2.0)) << r.diagnostics;
        EXPECT_LE(r.bias, s.eps / std::sqrt(2.0)) << r.diagnostics;

        // Reference: levels 0..8 (daily-and-finer steps), fixed paths.
        const MlmcResult ref =
            simulate_mlmc_fixed(product, *m, {400000, 200000, 100000, 50000, 25000, 12000, 6000, 3000, 1500}, 17);
        EXPECT_NEAR(r.value, ref.value, 3.0 * s.eps + 4.0 * ref.std_error)
            << r.diagnostics << " | reference " << ref.value << " +- " << ref.std_error;
    }

    // ── WP 26 P4b: Milstein ─────────────────────────────────────────────

    TEST(Mlmc, TheLocalVolSlopeIsTheLookupsDerivative)
    {
        const std::vector<Real> K = {50.0, 80.0, 100.0, 120.0, 200.0}, Tg = {0.0, 0.5, 2.0};
        const std::vector<Real> v = local_vol(false).sigma_loc_flat;
        const mc::GridView<Real> g{K.data(), 5, Tg.data(), 3, v.data()};
        for (const double S : {60.0, 90.0, 110.0, 150.0})
            for (const double t : {0.1, 0.7})
            {
                Real slope;
                const Real sigma = mc::local_vol_with_slope(g, S, t, slope);
                EXPECT_DOUBLE_EQ(sigma, mc::local_vol_bilinear(g, S, t));
                const double h = 1e-4;
                const double fd =
                    (mc::local_vol_bilinear(g, S + h, t) - mc::local_vol_bilinear(g, S - h, t)) / (2 * h);
                EXPECT_NEAR(slope, fd, 1e-9) << S << ' ' << t;
            }
        Real slope;
        mc::local_vol_with_slope(g, 300.0, 0.5, slope); // flat beyond the grid
        EXPECT_EQ(slope, 0.0);
    }

    TEST(Mlmc, MilsteinOnAFlatSurfaceIsLogEuler)
    {
        // No slope, no Milstein term: the same bits.
        ScriptModelSpec euler = local_vol(true), milstein = local_vol(true);
        milstein.milstein = true;
        const SimulationMCResult a = plain(kCall, euler, 0, 20000), b = plain(kCall, milstein, 0, 20000);
        EXPECT_EQ(a.npv(), b.npv());
    }

    TEST(Mlmc, MilsteinStaysOnTheCpu)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.milstein = true;
        auto m = make_script_model<Real>(spec);
        m->init(product.timeline(), product.defline());
        DeviceModel d;
        EXPECT_FALSE(m->describe_device(d));
        Real mean;
        EXPECT_FALSE(m->deflated_spot_mean(0, 0.5, mean)); // no longer an exact martingale
    }

    TEST(Mlmc, MilsteinLevelsDecayAsTheSquareOfTheStep)
    {
        // A local vol linear in S (two strike nodes far apart: no kink on
        // the paths' range): sigma smooth, Milstein's strong order 1, beta
        // ~ 2 -- against beta ~ 1 for log-Euler on the same surface.
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.K_grid = {10.0, 1000.0};
        spec.T_grid = {0.0, 2.0};
        spec.sigma_loc_flat = {0.50, 0.50, 0.05, 0.05}; // 50 % at 10, 5 % at 1000
        const std::vector<long long> n = {1000, 100000, 100000, 100000, 100000, 100000};
        const MlmcResult euler = simulate_mlmc_fixed(product, *make_script_model<Real>(spec), n, 29);
        spec.milstein = true;
        const MlmcResult milstein = simulate_mlmc_fixed(product, *make_script_model<Real>(spec), n, 29);
        RecordProperty("milstein_beta", std::to_string(milstein.beta));
        RecordProperty("euler_beta", std::to_string(euler.beta));
        EXPECT_NEAR(milstein.beta, 2.0, 0.3) << milstein.diagnostics;
        EXPECT_NEAR(euler.beta, 1.0, 0.25) << euler.diagnostics;
        // and the two schemes converge to the same price
        EXPECT_NEAR(milstein.value, euler.value, 4.0 * std::hypot(milstein.std_error, euler.std_error) + 0.01);
    }

    TEST(Mlmc, MilsteinCostGrowsAsEpsToTheMinusTwo)
    {
        // beta = 2 > gamma = 1: the MLMC cost is O(eps^-2) (Giles 2008,
        // Theorem 3.1), against O(eps^-3) for plain Monte Carlo with a first
        // order bias. Measured in log-log on the Milstein-GBM test bed.
        ScriptedProduct<Real> product(kSpot, ctx());
        const EulerGbm m(100.0, 0.5, 0.2, 1.0 / 12.0, true);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const std::vector<double> eps = {0.04, 0.02, 0.01, 0.005};
        for (const double e : eps)
        {
            MlmcSettings s;
            s.eps = e;
            s.seed = 31;
            const MlmcResult r = simulate_mlmc(product, m, s);
            EXPECT_TRUE(r.bias_converged) << r.diagnostics;
            EXPECT_NEAR(r.value, 100.0, 3.0 * e) << r.diagnostics;
            const double x = std::log(e), y = std::log(r.total_cost());
            sx += x, sy += y, sxx += x * x, sxy += x * y;
        }
        const double n = static_cast<double>(eps.size());
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        RecordProperty("cost_slope", std::to_string(slope));
        EXPECT_NEAR(slope, -2.0, 0.3);
    }

    // ── WP 26 P4c: opt-in through the script engine ─────────────────────

    TEST(Mlmc, TheScriptEngineRunsItWhenAsked)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.milstein = true;
        auto m = make_script_model<Real>(spec);
        PricingSettings s;
        s.mc_mlmc_eps = 0.02;
        s.mc_seed = 37;
        const SimulationMCResult r = simulate_script(product, *m, s);
        // Sobol, the default sampler: multilevel QMC (P5).
        EXPECT_NE(r.diagnostics.find("multilevel QMC"), std::string::npos) << r.diagnostics;
        EXPECT_LE(r.std_error(), 1.02 * 0.02 / std::sqrt(2.0));
        EXPECT_GT(r.n_paths, 0);
        // The reference price of the skewed call (P4a/P4b runs): 14.20.
        EXPECT_NEAR(r.npv(), 14.20, 3.0 * 0.02) << r.diagnostics;

        PricingSettings pseudo = s;
        pseudo.mc_sampler = SamplerKind::PseudoRandom;
        const SimulationMCResult p = simulate_script(product, *m, pseudo);
        EXPECT_NE(p.diagnostics.find("multilevel Monte Carlo"), std::string::npos) << p.diagnostics;
        EXPECT_NEAR(p.npv(), 14.20, 3.0 * 0.02) << p.diagnostics;
    }

    TEST(Mlmc, TheScriptEngineSaysWhyItDidNotRunIt)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        auto m = make_script_model<Real>(bs);
        PricingSettings s;
        s.mc_mlmc_eps = 0.02;
        s.mc_paths = 4096;
        const SimulationMCResult r = simulate_script(product, *m, s);
        EXPECT_NE(r.diagnostics.find("no multilevel Monte Carlo: the model has no time-step bias"),
                  std::string::npos)
            << r.diagnostics;

        PricingSettings negative;
        negative.mc_mlmc_eps = -1.0;
        EXPECT_THROW(simulate_script(product, *m, negative), InvalidInput);
    }

    // ── WP 26 P4d: approximate random variables (§8.7) ──────────────────

    TEST(Mlmc, ThePiecewiseLinearInverseCdfIsCloseAndExactInTheTail)
    {
        double err256 = 0.0, err4096 = 0.0;
        const PiecewiseLinearInverseCdf a256(256), a4096(4096);
        for (int i = 1; i < 100000; ++i)
        {
            const double p = i / 100000.0;
            err256 = std::max(err256, std::fabs(a256(p) - inverse_normal_cdf(p)));
            err4096 = std::max(err4096, std::fabs(a4096(p) - inverse_normal_cdf(p)));
        }
        EXPECT_LT(err4096, err256);
        EXPECT_LT(err256, 0.05);
        EXPECT_EQ(a256(1e-6), inverse_normal_cdf(1e-6)); // the extreme tail is exact
        EXPECT_EQ(a256(0.3), -a256(0.7));                // symmetric
    }

    TEST(Mlmc, TheApproximateLevelPlusItsCorrectionIsUnbiased)
    {
        // E[P_exact] = E[P_approx] + E[P_exact - P_approx] whatever the
        // approximation: a coarse table, a Black-Scholes call.
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        auto m = make_script_model<Real>(bs);
        const ApproxRvResult r =
            simulate_approx_inverse_cdf(product, *m, PiecewiseLinearInverseCdf(16), 400000, 100000, 400000, 41);
        const double exact_se = std::sqrt(r.exact.stats.variance() / static_cast<double>(r.exact.stats.n));
        EXPECT_NEAR(r.value, r.exact.stats.mean(), 4.0 * std::hypot(r.std_error, exact_se));
        // The correction is small against the price's own variance.
        EXPECT_LT(r.correction.stats.variance(), 1e-3 * r.exact.stats.variance());
    }

    TEST(Mlmc, TheCorrectionVarianceFallsWithTheTablesResolution)
    {
        // Measured, not assumed (§8.7): 256 -> 4096 cells.
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        double v[2];
        int i = 0;
        for (const int cells : {256, 4096})
        {
            auto m = make_script_model<Real>(bs);
            v[i++] = simulate_approx_inverse_cdf(product, *m, PiecewiseLinearInverseCdf(cells), 1000, 200000, 1000, 43)
                         .correction.stats.variance();
        }
        EXPECT_LT(v[1], v[0] / 4.0) << v[0] << " -> " << v[1];
    }

    // ── WP 26 P5: multilevel QMC (§9) ────────────────────────────────────

    TEST(Mlmc, MultilevelQmcIsMultilevelMcWithinErrorAndFarCheaper)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.milstein = true;
        auto m = make_script_model<Real>(spec);
        MlmcSettings a;
        a.eps = 0.02;
        a.seed = 13;
        const MlmcResult mc = simulate_mlmc(product, *m, a);
        MlqmcSettings b;
        b.eps = 0.02;
        b.seed = 13;
        const MlmcResult qmc = simulate_mlqmc(product, *m, b);
        EXPECT_TRUE(qmc.bias_converged) << qmc.diagnostics;
        EXPECT_LE(qmc.std_error, 1.0 * b.eps / std::sqrt(2.0)) << qmc.diagnostics;
        // Both estimate E[P] within eps; their finest levels may differ.
        EXPECT_NEAR(qmc.value, mc.value, 3.0 * a.eps) << qmc.diagnostics << " | " << mc.diagnostics;
        // Measured 14x cheaper on this case (WP 26 §9.1).
        EXPECT_LT(5.0 * qmc.total_cost(), mc.total_cost()) << qmc.diagnostics << " | " << mc.diagnostics;
    }

    TEST(Mlmc, MultilevelQmcCostGrowsAsEpsToTheMinusOneOnASmoothSurface)
    {
        // Giles & Waterhouse 2009: with Milstein on a smooth volatility the
        // coarse, low-dimensional levels carry the cost and QMC removes most
        // of it. Measured slope -1.01, against -2.00 for MLMC (§9.1).
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.K_grid = {10.0, 1000.0};
        spec.T_grid = {0.0, 2.0};
        spec.sigma_loc_flat = {0.50, 0.50, 0.05, 0.05};
        spec.milstein = true;
        auto m = make_script_model<Real>(spec);
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const std::vector<double> eps = {0.02, 0.01, 0.005, 0.0025};
        for (const double e : eps)
        {
            MlqmcSettings s;
            s.eps = e;
            s.seed = 13;
            const MlmcResult r = simulate_mlqmc(product, *m, s);
            EXPECT_TRUE(r.bias_converged) << r.diagnostics;
            const double x = std::log(e), y = std::log(r.total_cost());
            sx += x, sy += y, sxx += x * x, sxy += x * y;
        }
        const double n = static_cast<double>(eps.size());
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        RecordProperty("mlqmc_cost_slope", std::to_string(slope));
        EXPECT_LT(slope, -0.7);
        EXPECT_GT(slope, -1.5);
    }

    TEST(Mlmc, MultilevelQmcRefusesWhatSobolCannotDraw)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        ScriptModelSpec spec = local_vol(false);
        spec.max_dt = 1.0 / 30000.0; // level 0 alone draws 30 000 numbers a path
        auto m = make_script_model<Real>(spec);
        MlqmcSettings s;
        s.eps = 0.1;
        EXPECT_THROW(simulate_mlqmc(product, *m, s), InvalidInput);
    }

    TEST(Mlmc, TheBiasTestReadsTheLastThreeCorrections)
    {
        // SPY's corrections fell fast then slowly (WP 26 §9.2): the
        // two-level test, max(|Y_{L-1}| / r, |Y_L|) / (r - 1), stopped at
        // level 2; the last three see the level-1 correction still large.
        const std::vector<double> spy = {54.0, 1.06, 0.12};
        EXPECT_NEAR(mlmc_detail::remaining_bias(spy, 1.0), std::max(1.06 / 2.0, 0.12), 1e-12);
        const std::vector<double> later = {54.0, 1.06, 0.12, 0.03, 0.06};
        EXPECT_NEAR(mlmc_detail::remaining_bias(later, 1.0), std::max({0.12 / 4.0, 0.03 / 2.0, 0.06}), 1e-12);
        // r = 4 (alpha = 2): divided by r - 1 = 3.
        EXPECT_NEAR(mlmc_detail::remaining_bias({1.0, 0.4, 0.1}, 2.0), 0.1 / 3.0, 1e-12);
    }

    TEST(Mlmc, MultilevelQmcGivesTheSameBitsOnAnyNumberOfThreads)
    {
        // Each replicate sums its points in order on its own (WP 26 §9.4).
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        MlqmcSettings s;
        s.eps = 0.02;
        s.seed = 13;
        const MlmcResult one = simulate_mlqmc(product, *m, s);
        ThreadPool pool;
        pool.start(7);
        const MlmcResult eight = simulate_mlqmc(product, *m, s, &pool);
        EXPECT_EQ(one.value, eight.value);
        EXPECT_EQ(one.std_error, eight.std_error);
        EXPECT_EQ(one.finest_level(), eight.finest_level());
    }

    TEST(Mlmc, MultilevelQmcAimsAtAShareOfThePrice)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        MlqmcSettings s;
        s.eps = 1e-6; // the floor
        s.rel_eps = 0.002;
        s.seed = 13;
        const MlmcResult r = simulate_mlqmc(product, *m, s);
        EXPECT_NEAR(r.eps, 0.002 * r.value, 1e-12);
        EXPECT_LE(r.std_error, 1.02 * r.eps / std::sqrt(2.0)) << r.diagnostics;
        EXPECT_LE(r.bias, r.eps / std::sqrt(2.0)) << r.diagnostics;
    }

    // ── WP 26 §9.4: the default route's bias, shown ──────────────────────

    TEST(Mlmc, APlainLocalVolRunEstimatesItsBiasByHalvingTheSteps)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        PricingSettings s;
        s.mc_paths = 1 << 16;
        s.mc_bias_estimate = true;
        const SimulationMCResult r = simulate_script(product, *m, s);
        ASSERT_TRUE(r.discretisation_bias.has_value()) << r.diagnostics;
        // The price is the halved run's; the bias the difference from the
        // unhalved one.
        auto halved = make_script_model<Real>(local_vol(false));
        ASSERT_TRUE(halved->set_refinement(2));
        PricingSettings p = s;
        p.mc_bias_estimate = false;
        const SimulationMCResult fine = simulate_script(product, *halved, p);
        const SimulationMCResult coarse = simulate_script(product, *m, p);
        EXPECT_EQ(r.npv(), fine.npv());
        EXPECT_EQ(*r.discretisation_bias, std::fabs(fine.npv() - coarse.npv()));
        EXPECT_GT(*r.discretisation_bias, 0.0);
    }

    TEST(Mlmc, BlackScholesHasNoDiscretisationBiasAndRoughBergomisIsNotEstimated)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        PricingSettings s;
        s.mc_paths = 1 << 14;
        s.mc_bias_estimate = true;
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        auto m = make_script_model<Real>(bs);
        const SimulationMCResult exact = simulate_script(product, *m, s);
        ASSERT_TRUE(exact.discretisation_bias.has_value());
        EXPECT_EQ(*exact.discretisation_bias, 0.0);

        // The hybrid scheme's grid does not refine (WP 26 §9.5).
        ScriptModelSpec rb = local_vol(true);
        rb.model = "rough_bergomi";
        rb.xi_times = {0.5, 2.0};
        rb.xi_values = {0.04, 0.04};
        auto r = make_script_model<Real>(rb);
        const SimulationMCResult unknown = simulate_script(product, *r, s);
        EXPECT_FALSE(unknown.discretisation_bias.has_value());
        EXPECT_NE(unknown.diagnostics.find("not estimated"), std::string::npos) << unknown.diagnostics;
    }

    // ── WP 26 §9.5: the stochastic-volatility models refine ──────────────

    namespace
    {
        ScriptModelSpec heston()
        {
            ScriptModelSpec h = local_vol(true);
            h.model = "heston";
            h.heston = HestonParams{0.04, 2.0, 0.04, 0.5, -0.7};
            return h;
        }
    } // namespace

    TEST(Mlmc, StochasticVolatilityModelsRefineTheirGrids)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto check = [&](std::unique_ptr<ISimulationModel<Real>> m, const char *name)
        {
            m->init(product.timeline(), product.defline());
            const std::size_t base = m->sim_dim();
            auto r = m->clone();
            ASSERT_TRUE(r->set_refinement(4)) << name;
            r->init(product.timeline(), product.defline());
            EXPECT_EQ(r->sim_dim(), 4 * base) << name;
        };
        check(make_script_model<Real>(heston()), "heston");
        check(std::make_unique<SABRSimModel<Real>>(100.0, 0.03, 0.3, 0.7, -0.3, 0.4, 1.0 / 12.0), "sabr");
        ScriptModelSpec slv = heston();
        slv.model = "slv";
        slv.K_grid = {50.0, 200.0};
        slv.T_grid = {0.0, 2.0};
        slv.leverage_flat = {1.0, 1.0, 1.0, 1.0};
        check(make_script_model<Real>(slv), "slv");
    }

    TEST(Mlmc, JumpsStayOutOfTheLevels)
    {
        // Bates with jumps: their draws would couple poorly (WP 26 §8.3).
        BatesSimModel<Real> bates(100.0, 0.03, 0.0, 0.04, 2.0, 0.04, 0.5, -0.7, 0.5, -0.1, 0.1, 1.0 / 12.0);
        EXPECT_FALSE(bates.set_refinement(2));
        EXPECT_TRUE(bates.set_refinement(1));
    }

    TEST(Mlmc, UnderHestonTheTelescopingSumIsPlainMonteCarloOnTheFinestGrid)
    {
        // The jump draws a Heston step carries (Bates with lambda = 0) are
        // copied to the coarse step: unused, the levels stay exact.
        ScriptedProduct<Real> product(kPut, ctx());
        auto m = make_script_model<Real>(heston());
        const MlmcResult ml = simulate_mlmc_fixed(product, *m, {200000, 40000, 20000}, 47);
        const SimulationMCResult pl = plain(kPut, heston(), 2, 200000);
        EXPECT_NEAR(ml.value, pl.npv(), 4.0 * std::hypot(ml.std_error, pl.std_error())) << ml.diagnostics;
        EXPECT_GT(ml.levels[1].variance(), 0.0);
        EXPECT_LT(ml.levels[2].variance(), ml.levels[1].variance()) << ml.diagnostics;
    }

    TEST(Mlmc, AHestonRunNowEstimatesItsBias)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(heston());
        PricingSettings s;
        s.mc_paths = 1 << 14;
        s.mc_bias_estimate = true;
        const SimulationMCResult r = simulate_script(product, *m, s);
        ASSERT_TRUE(r.discretisation_bias.has_value()) << r.diagnostics;
        EXPECT_GT(*r.discretisation_bias, 0.0);
    }

    TEST(Mlmc, TheMultilevelRouteReportsItsBiasAndAimsAtAShareOfThePrice)
    {
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        PricingSettings s;
        s.mc_mlmc_eps = 1e-4; // floor
        s.mc_mlmc_rel_eps = 0.002;
        ThreadPool pool;
        pool.start(7);
        const SimulationMCResult r = simulate_script(product, *m, s, &pool);
        ASSERT_TRUE(r.discretisation_bias.has_value()) << r.diagnostics;
        EXPECT_LE(*r.discretisation_bias, 0.002 * r.npv() / std::sqrt(2.0) * 1.0001) << r.diagnostics;
        EXPECT_LE(r.std_error(), 1.02 * 0.002 * r.npv() / std::sqrt(2.0)) << r.diagnostics;

        PricingSettings no_floor = s;
        no_floor.mc_mlmc_eps = 0.0;
        EXPECT_THROW(simulate_script(product, *m, no_floor), InvalidInput);
    }

    // ── WP 26 P6: the levels coupled through the Sobol point (§10) ──────

    TEST(Mlmc, CoupledSobolTablesGiveBothGridsTheSameBrownianPath)
    {
        // An uneven coarse grid, each step halved, two factors.
        BrownianLayout coarse{{0.1, 0.35, 0.5, 1.0, 1.25}, 2, 2}, fine{{}, 2, 2};
        Time prev = 0.0;
        for (const Time t : coarse.times)
        {
            fine.times.push_back(0.5 * (prev + t));
            fine.times.push_back(t);
            prev = t;
        }
        const std::vector<uint64_t> keys{11, 12, 13};
        const mc::CoupledSobolTables t = mc::coupled_sobol_tables(fine, coarse, keys);
        const std::size_t nf = fine.times.size(), nc = coarse.times.size();
        ASSERT_EQ(t.fine.dim, static_cast<int>(2 * nf));
        ASSERT_EQ(t.coarse.dim, static_cast<int>(2 * nc));

        std::vector<double> zf(2 * nf), zc(2 * nc), pt(2 * nf);
        std::vector<double> moment(4 * nf * nf, 0.0); // E[z_i z_j] over the fine draws
        const uint32_t n_points = 1u << 14;
        for (uint64_t b = 0; b < keys.size(); ++b)
            for (uint32_t p = 0; p < n_points; ++p)
            {
                mc::sobol_bridged_gaussians(t.fine.view(), b, p, pt.data(), 1, zf.data(), 1);
                mc::sobol_bridged_gaussians(t.coarse.view(), b, p, pt.data(), 1, zc.data(), 1);
                // Each coarse increment is the sum of its two fine ones.
                Time t0 = 0.0;
                for (std::size_t s = 0; s < nc; ++s)
                {
                    const Time tm = fine.times[2 * s], t1 = fine.times[2 * s + 1];
                    for (std::size_t f = 0; f < 2; ++f)
                        ASSERT_NEAR(zc[2 * s + f] * std::sqrt(t1 - t0),
                                    zf[2 * (2 * s) + f] * std::sqrt(tm - t0) +
                                        zf[2 * (2 * s + 1) + f] * std::sqrt(t1 - tm),
                                    1e-12);
                    t0 = t1;
                }
                if (b == 0)
                    for (std::size_t i = 0; i < 2 * nf; ++i)
                        for (std::size_t j = 0; j < 2 * nf; ++j)
                            moment[i * 2 * nf + j] += zf[i] * zf[j] / n_points;
            }
        // The fine draws are still independent N(0, 1): the nested order is
        // a Brownian bridge like any other.
        for (std::size_t i = 0; i < 2 * nf; ++i)
            for (std::size_t j = 0; j < 2 * nf; ++j)
                EXPECT_NEAR(moment[i * 2 * nf + j], i == j ? 1.0 : 0.0, 0.03) << i << ' ' << j;

        // The coarse grid's bridge is its own: the coupling does not change
        // the coarse level's construction.
        const mc::SobolTables own = mc::sobol_tables(2 * nc, keys, coarse, true);
        EXPECT_EQ(t.coarse.bridge_index, own.bridge_index);
        EXPECT_EQ(t.coarse.std_dev, own.std_dev);
    }

    TEST(Mlmc, CoupledSobolTablesRefuseGridsThatDoNotNest)
    {
        const std::vector<uint64_t> keys{1, 2};
        const BrownianLayout coarse{{0.5, 1.0}, 1, 1};
        EXPECT_THROW(mc::coupled_sobol_tables(BrownianLayout{{0.25, 0.5, 1.0}, 1, 1}, coarse, keys), InvalidInput);
        EXPECT_THROW(mc::coupled_sobol_tables(BrownianLayout{{0.25, 0.6, 0.75, 1.0}, 1, 1}, coarse, keys),
                     InvalidInput);
        // A draw that is not a Brownian increment (a jump's).
        EXPECT_THROW(mc::coupled_sobol_tables(BrownianLayout{{0.25, 0.5, 0.75, 1.0}, 1, 2},
                                              BrownianLayout{{0.5, 1.0}, 1, 2}, keys),
                     InvalidInput);
    }

    TEST(Mlmc, WithoutACudaDeviceTheMultilevelRouteStaysOnTheCpuAndSaysSo)
    {
        if (gpu::device_count() > 0)
            GTEST_SKIP() << "a CUDA device is present (tests/gpu/testGpuMlqmc.cpp covers it)";
        ScriptedProduct<Real> product(kCall, ctx());
        auto m = make_script_model<Real>(local_vol(false));
        PricingSettings s;
        s.mc_mlmc_eps = 0.05;
        s.mc_device = ComputeDevice::Auto;
        const SimulationMCResult r = simulate_script(product, *m, s);
        EXPECT_EQ(r.device, "cpu");
        EXPECT_NE(r.diagnostics.find("multilevel QMC"), std::string::npos) << r.diagnostics;
    }

} // namespace quantModeling
