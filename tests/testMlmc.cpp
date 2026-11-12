#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
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
         * (alpha = 1), and whose strong error gives beta = 1.
         */
        class EulerGbm final : public ISimulationModel<Real>
        {
          public:
            EulerGbm(Real s0, Real r, Real sigma, Time max_dt)
                : s0_(s0), r_(r), sigma_(sigma), max_dt_(max_dt) {}
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
                    S *= 1.0 + r_ * h + sigma_ * std::sqrt(h) * z[k];
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
                        4.0 * std::sqrt(r.levels[l].variance() / r.levels[l].n))
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

} // namespace quantModeling
