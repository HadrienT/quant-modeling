#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/thread_pool.hpp"

// The generic engine on several CPU threads (blueprint/wp/26-performance.md,
// P1e): the Sobol replicates and the counter-based logical blocks run as
// tasks and fold in a fixed order, so every result is the same bits on 1, 2,
// 16 or 56 threads. What cannot run concurrently stays on the calling thread
// and says nothing about threads.

namespace quantModeling
{
    namespace
    {
        std::string library(const std::string &name)
        {
            std::ifstream in(std::filesystem::path(QM_PRODUCT_LIBRARY_DIR) / (name + ".qms"));
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};

        using ModelFactory = std::function<std::unique_ptr<ISimulationModel<Real>>()>;

        ModelFactory black_scholes()
        {
            return []
            { return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25); };
        }

        ModelFactory local_vol()
        {
            return []
            {
                const std::vector<Real> K{60, 80, 90, 100, 110, 120, 150}, T{0.25, 0.5, 1.0, 2.0, 5.0};
                std::vector<Real> grid;
                for (Real k : K)
                    for (Real t : T)
                        grid.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
                return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, K, T, grid, 1.0 / 24.0);
            };
        }

        ModelFactory three_assets()
        {
            return []
            {
                Eigen::MatrixXd corr(3, 3);
                corr << 1.0, 0.5, 0.4, 0.5, 1.0, 0.3, 0.4, 0.3, 1.0;
                return std::make_unique<MultiAssetBSSimModel<Real>>(std::vector<Real>{100, 100, 100}, 0.03,
                                                                    std::vector<Real>{0.01, 0.0, 0.02},
                                                                    std::vector<Real>{0.2, 0.25, 0.3}, corr);
            };
        }

        PricingSettings settings(SamplerKind sampler, RngKind rng = RngKind::Philox)
        {
            PricingSettings s;
            s.mc_paths = 1 << 15;
            s.mc_seed = 11;
            s.mc_sampler = sampler;
            s.mc_rng = rng;
            return s;
        }

        /// workers = 0: no pool at all.
        SimulationMCResult run(const ScriptedProduct<Real> &product, const ModelFactory &make,
                               const PricingSettings &s, std::size_t workers)
        {
            auto model = make();
            if (workers == 0)
                return simulate<Real>(product, *model, s);
            ThreadPool pool;
            pool.start(workers);
            SimulationMCResult r = simulate<Real>(product, *model, s, &pool);
            pool.stop();
            return r;
        }

        /// The same bits without a pool and on 2, 16 and 56 threads, and the
        /// threaded runs say they were threaded.
        void expect_thread_count_invariant(const std::string &what, const ScriptedProduct<Real> &product,
                                           const ModelFactory &make, const PricingSettings &s)
        {
            const SimulationMCResult one = run(product, make, s, 0);
            ASSERT_FALSE(one.values.empty()) << what;
            EXPECT_EQ(one.diagnostics.find("threads"), std::string::npos) << what;
            for (const std::size_t workers : {1u, 15u, 55u})
            {
                const SimulationMCResult many = run(product, make, s, workers);
                EXPECT_EQ(many.values, one.values) << what << ", " << workers + 1 << " threads";
                EXPECT_EQ(many.std_errors, one.std_errors) << what << ", " << workers + 1 << " threads";
                EXPECT_EQ(many.n_paths, one.n_paths) << what;
                EXPECT_NE(many.diagnostics.find("on " + std::to_string(workers + 1) + " threads"), std::string::npos)
                    << what << ": " << many.diagnostics;
            }
        }
    } // namespace

    TEST(SimulationThreads, PhiloxUnderLocalVolIsTheSameBitsOnAnyThreadCount)
    {
        const ScriptedProduct<Real> product(library("up-and-out-call"), kCtx);
        expect_thread_count_invariant("Philox, local vol", product, local_vol(), settings(SamplerKind::PseudoRandom));
    }

    TEST(SimulationThreads, SobolWithTheBridgeIsTheSameBitsOnAnyThreadCount)
    {
        const ScriptedProduct<Real> product(library("asian-call"), kCtx);
        expect_thread_count_invariant("Sobol + bridge, BS", product, black_scholes(), settings(SamplerKind::Sobol));
    }

    TEST(SimulationThreads, ControlVariatesAreTheSameBitsOnAnyThreadCount)
    {
        const ScriptedProduct<Real> product(library("worst-of-call"), kCtx);
        PricingSettings philox = settings(SamplerKind::PseudoRandom);
        philox.mc_spot_control = true;
        expect_thread_count_invariant("Philox + spot control, three assets", product, three_assets(), philox);
        PricingSettings sobol = settings(SamplerKind::Sobol);
        sobol.mc_spot_control = true;
        expect_thread_count_invariant("Sobol + spot control, three assets", product, three_assets(), sobol);
    }

    TEST(SimulationThreads, StratifiedAndImportanceSamplingAreTheSameBitsOnAnyThreadCount)
    {
        const ScriptedProduct<Real> call(library("european-call"), kCtx);
        expect_thread_count_invariant("stratified, BS", call, black_scholes(), settings(SamplerKind::Stratified));

        const ScriptedProduct<Real> digital("2027-06-01\n    if spot() > 180 then pays 100 endIf\n", kCtx);
        PricingSettings is = settings(SamplerKind::PseudoRandom);
        is.mc_importance_drift = true;
        expect_thread_count_invariant("importance sampling, BS", digital, black_scholes(), is);
    }

    TEST(SimulationThreads, WhatCannotRunConcurrentlyStaysOnOneThread)
    {
        // PCG32 is one stream; the tree evaluator (bytecode off, or early
        // exercise) keeps one evaluator per product. Both run on the calling
        // thread with a pool present, and give the bits they give without one.
        const ScriptedProduct<Real> hard(library("up-and-out-call"), kCtx);
        const PricingSettings pcg = settings(SamplerKind::PseudoRandom, RngKind::Pcg32);
        ScriptSettings no_bytecode;
        no_bytecode.bytecode = false;
        const ScriptedProduct<Real> tree(library("up-and-out-call"), kCtx, no_bytecode);
        const PricingSettings philox = settings(SamplerKind::PseudoRandom);
        for (const auto &[what, product, s] :
             {std::tuple{"PCG32", &hard, pcg}, std::tuple{"tree evaluator", &tree, philox}})
        {
            const SimulationMCResult one = run(*product, black_scholes(), s, 0);
            const SimulationMCResult many = run(*product, black_scholes(), s, 15);
            EXPECT_EQ(many.values, one.values) << what;
            EXPECT_EQ(many.std_errors, one.std_errors) << what;
            EXPECT_EQ(many.diagnostics.find("threads"), std::string::npos) << what << ": " << many.diagnostics;
        }
    }

    TEST(SimulationThreads, AnErrorInAWorkerReachesTheCaller)
    {
        // spot(1) on a one-asset model throws inside the payoff, on whichever
        // thread draws the path: the caller gets the error, after every task
        // has stopped reading the engine's frame.
        const ScriptedProduct<Real> product("2027-06-01\n    pays spot(1)\n", kCtx);
        for (const SamplerKind sampler : {SamplerKind::PseudoRandom, SamplerKind::Sobol})
            EXPECT_THROW(run(product, black_scholes(), settings(sampler), 15), InvalidInput);
    }

} // namespace quantModeling
