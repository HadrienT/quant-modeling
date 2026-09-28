#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/path_draws.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

// Lot G3 of blueprint/wp/19-gpu.md §2.5: the generic variance reduction of
// the simulation engine, for any script --
//   - stratified terminal value, the path filled in by the conditional
//     Brownian bridge: still a Brownian path (covariance min(s, t)), one
//     point per stratum;
//   - spot control variates, regressed out;
// each unbiased (the price agrees with the plain run) and each measured:
// its standard error against the plain run's at the same number of paths,
// without antithetic pairs. The thresholds sit well under what
// benchmarks/gpu_variance_reduction.cpp measures over 40 seeds.

namespace quantModeling
{
    namespace
    {
        std::string script(const std::string &name)
        {
            std::ifstream in(std::filesystem::path(QM_PRODUCT_LIBRARY_DIR) / (name + ".qms"));
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};

        PricingSettings settings(SamplerKind sampler, bool control)
        {
            PricingSettings s;
            s.mc_paths = 1 << 16;
            s.mc_seed = 7;
            s.mc_antithetic = false;
            s.mc_rng = RngKind::Philox;
            s.mc_sampler = sampler;
            s.mc_spot_control = control;
            return s;
        }

        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};
        std::vector<Real> skewed()
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
            return g;
        }
    } // namespace

    // Glasserman §4.3.2: stratify W(T), fill in by the bridge. The law is
    // Brownian's: E W(s) W(t) = min(s, t).
    TEST(StratifiedDraws, ConditionalBridgeIsBrownian)
    {
        const std::vector<Time> times{0.1, 0.35, 0.5, 0.8, 1.0};
        const std::size_t n = times.size();
        mc::PathDraws draws;
        draws.seed = 3;
        draws.strata = 64;
        draws.times = times.data();
        draws.n_steps = static_cast<int>(n);

        const int reps = 400;
        std::vector<std::vector<double>> sum(n, std::vector<double>(n, 0.0));
        std::vector<double> mean(n, 0.0);
        for (int b = 0; b < reps; ++b)
            for (uint64_t i = 0; i < draws.strata; ++i)
            {
                draws.begin(static_cast<uint64_t>(b) * draws.strata + i, i);
                std::vector<double> W(n);
                double w = 0.0, t_prev = 0.0;
                for (std::size_t d = 0; d < n; ++d)
                {
                    w += std::sqrt(times[d] - t_prev) * draws(static_cast<int>(d), 0);
                    t_prev = times[d];
                    W[d] = w;
                }
                // One point per stratum of W(T).
                const double u = norm_cdf(W[n - 1] / std::sqrt(times[n - 1]));
                EXPECT_GE(u * static_cast<double>(draws.strata), static_cast<double>(i) - 1e-9);
                EXPECT_LE(u * static_cast<double>(draws.strata), static_cast<double>(i + 1) + 1e-9);
                for (std::size_t a = 0; a < n; ++a)
                {
                    mean[a] += W[a];
                    for (std::size_t c = 0; c < n; ++c)
                        sum[a][c] += W[a] * W[c];
                }
            }
        const double N = reps * static_cast<double>(draws.strata);
        for (std::size_t a = 0; a < n; ++a)
        {
            EXPECT_NEAR(mean[a] / N, 0.0, 4.0 * std::sqrt(times[a] / N)) << a;
            for (std::size_t c = 0; c < n; ++c)
                EXPECT_NEAR(sum[a][c] / N, std::min(times[a], times[c]), 0.03) << a << "," << c;
        }
    }

    namespace
    {
        struct Runs
        {
            SimulationMCResult plain, strat, control, both;
        };

        template <class MakeModel>
        Runs run_all(const std::string &name, MakeModel make)
        {
            const ScriptedProduct<Real> product(script(name), kCtx);
            Runs r;
            auto m1 = make();
            r.plain = simulate<Real>(product, *m1, settings(SamplerKind::PseudoRandom, false));
            auto m2 = make();
            r.strat = simulate<Real>(product, *m2, settings(SamplerKind::Stratified, false));
            auto m3 = make();
            r.control = simulate<Real>(product, *m3, settings(SamplerKind::PseudoRandom, true));
            auto m4 = make();
            r.both = simulate<Real>(product, *m4, settings(SamplerKind::Stratified, true));
            // Every estimator is unbiased: each agrees with the plain run.
            for (const SimulationMCResult *x : {&r.strat, &r.control, &r.both})
            {
                const double se = std::hypot(x->std_error(), r.plain.std_error());
                EXPECT_NEAR(x->npv(), r.plain.npv(), 4.0 * se) << name << ": " << x->diagnostics;
            }
            return r;
        }

        double variance_ratio(const SimulationMCResult &plain, const SimulationMCResult &x)
        {
            const double r = (plain.std_error() * plain.std_error()) / (x.std_error() * x.std_error());
            std::printf("  variance / %.2f  %s\n", r, x.diagnostics.c_str());
            return r;
        }
    } // namespace

    // A forward-start call: its payoff reads two spots, the stratified
    // terminal value and the controls both see them.
    TEST(GenericVarianceReduction, EuropeanCallUnderBlackScholes)
    {
        const Runs r = run_all("european-call", []
                               { return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25); });
        EXPECT_GT(variance_ratio(r.plain, r.strat), 2.0) << r.strat.diagnostics;
        EXPECT_GT(variance_ratio(r.plain, r.control), 2.0) << r.control.diagnostics;
        EXPECT_GT(variance_ratio(r.plain, r.both), variance_ratio(r.plain, r.control)) << r.both.diagnostics;
    }

    TEST(GenericVarianceReduction, AsianCallUnderLocalVol)
    {
        const Runs r = run_all("asian-call", []
                               { return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed()); });
        // Twelve fixings: the terminal value carries a smaller share (the
        // benchmark measures / 1.5 over 40 seeds); the average is what the
        // controls capture.
        EXPECT_GT(variance_ratio(r.plain, r.strat), 1.15) << r.strat.diagnostics;
        EXPECT_GT(variance_ratio(r.plain, r.control), 2.5) << r.control.diagnostics;
    }

    TEST(GenericVarianceReduction, AutocallUnderLocalVol)
    {
        const Runs r = run_all("phoenix-autocall", []
                               { return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed()); });
        // Digital coupons and a knock-in: little linear in the spots (/ 1.3
        // measured), much in the terminal value's stratum.
        EXPECT_GT(variance_ratio(r.plain, r.control), 1.05) << r.control.diagnostics;
        EXPECT_GT(variance_ratio(r.plain, r.both), 1.5) << r.both.diagnostics;
    }

    // The regression control does not bias: across seeds its estimate
    // centres on the plain one, and a spot-only payoff is fully explained.
    TEST(GenericVarianceReduction, ControlOfTheSpotItselfRemovesAllVariance)
    {
        const std::string forward = "2027-06-01\n    pays spot()\n";
        const ScriptedProduct<Real> product(forward, kCtx);
        BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
        const SimulationMCResult r = simulate<Real>(product, m, settings(SamplerKind::PseudoRandom, true));
        const double t = kCtx.t(Date::from_iso("2027-06-01"));
        EXPECT_NEAR(r.npv(), 100.0 * std::exp(-0.01 * t), 1e-9);
        EXPECT_LT(r.std_error(), 1e-7); // rounding in the co-moments only
    }

} // namespace quantModeling
