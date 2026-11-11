#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/thread_pool.hpp"

// Lot P1a of blueprint/wp/26-performance.md: the CPU's batched interpreter
// (several paths per decoded instruction, hard ifs as masks) gives the
// path-by-path engine's result bit for bit -- every library script, BS,
// local vol, Heston and multi-asset BS, Philox (antithetic or not, with an
// importance-sampling drift), stratified and Sobol, with partial batches at
// the end, on one thread and on a pool.

namespace quantModeling
{
    namespace
    {
        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

        struct Script
        {
            std::string name, source;
        };

        std::vector<Script> library()
        {
            std::vector<Script> out;
            for (const auto &entry : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
            {
                if (entry.path().extension() != ".qms")
                    continue;
                std::ifstream in(entry.path());
                std::stringstream text;
                text << in.rdbuf();
                out.push_back({entry.path().stem().string(), text.str()});
            }
            std::sort(out.begin(), out.end(), [](const Script &a, const Script &b)
                      { return a.name < b.name; });
            return out;
        }

        enum class Dyn
        {
            BlackScholes,
            LocalVol,
            Heston
        };

        std::unique_ptr<ISimulationModel<Real>> model(Dyn dyn, std::size_t n_assets)
        {
            if (n_assets > 1)
            {
                const auto n = static_cast<Eigen::Index>(n_assets);
                Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(n, n, 0.4);
                corr.diagonal().setOnes();
                std::vector<Real> s0, q, vol;
                for (std::size_t i = 0; i < n_assets; ++i)
                {
                    s0.push_back(100.0 - 4.0 * static_cast<double>(i));
                    q.push_back(0.01);
                    vol.push_back(0.2 + 0.04 * static_cast<double>(i));
                }
                return std::make_unique<MultiAssetBSSimModel<Real>>(s0, 0.03, q, vol, corr);
            }
            switch (dyn)
            {
                case Dyn::LocalVol:
                {
                    std::vector<Real> g;
                    for (Real k : kK)
                        for (Real t : kT)
                            g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
                    return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, g);
                }
                case Dyn::Heston:
                    return std::make_unique<BatesSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0,
                                                                 0.0, 0.0);
                default:
                    return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
            }
        }

        bool same_bits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }

        /// Batched against path by path on every library script; returns how
        /// many scripts actually ran batched.
        int expect_batch_equals_paths(Dyn dyn, PricingSettings s, ThreadPool *pool = nullptr)
        {
            int batched = 0;
            for (const Script &sc : library())
            {
                const ScriptedProduct<Real> product(sc.source, kCtx);
                if (dyn != Dyn::BlackScholes && product.n_underlyings() > 1)
                    continue;
                if (product.batch_width() > 0)
                    ++batched;
                auto m1 = model(dyn, product.n_underlyings());
                auto m2 = model(dyn, product.n_underlyings());
                PricingSettings off = s;
                off.mc_batch = false;
                const SimulationMCResult a = simulate_script(product, *m1, off, pool);
                const SimulationMCResult b = simulate_script(product, *m2, s, pool);
                EXPECT_TRUE(same_bits(a.npv(), b.npv()))
                    << sc.name << ": path by path " << a.npv() << ", batched " << b.npv();
                EXPECT_TRUE(same_bits(a.std_error(), b.std_error())) << sc.name;
            }
            return batched;
        }

        PricingSettings philox(int paths)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 11;
            s.mc_antithetic = true;
            s.mc_rng = RngKind::Philox;
            return s;
        }
    } // namespace

    // 2 x 4 099 units: a full logical block and a partial one, whose last
    // batch is partial too.
    TEST(ScriptBatch, BlackScholesPhiloxAntithetic)
    {
        EXPECT_GT(expect_batch_equals_paths(Dyn::BlackScholes, philox(2 * 4099)), 30);
    }

    TEST(ScriptBatch, BlackScholesPhiloxPlain)
    {
        PricingSettings s = philox(4099 + 5);
        s.mc_antithetic = false;
        expect_batch_equals_paths(Dyn::BlackScholes, s);
    }

    TEST(ScriptBatch, LocalVolAndHestonPhilox)
    {
        expect_batch_equals_paths(Dyn::LocalVol, philox(2 * 1003));
        expect_batch_equals_paths(Dyn::Heston, philox(2 * 1003));
    }

    TEST(ScriptBatch, StratifiedAndImportanceSampling)
    {
        PricingSettings s = philox(2 * 1003);
        s.mc_sampler = SamplerKind::Stratified;
        expect_batch_equals_paths(Dyn::BlackScholes, s);
        PricingSettings is = philox(2 * 1003);
        is.mc_importance_drift = true;
        expect_batch_equals_paths(Dyn::BlackScholes, is);
    }

    // Sobol: 4 replicates of 1 003 points, the last batch of each partial.
    TEST(ScriptBatch, Sobol)
    {
        PricingSettings s;
        s.mc_paths = 4 * 1003;
        s.mc_seed = 11;
        s.mc_rqmc_batches = 4;
        s.mc_sampler = SamplerKind::Sobol;
        expect_batch_equals_paths(Dyn::BlackScholes, s);
        expect_batch_equals_paths(Dyn::LocalVol, s);
    }

    TEST(ScriptBatch, OnAPool)
    {
        ThreadPool pool;
        pool.start(3);
        expect_batch_equals_paths(Dyn::BlackScholes, philox(2 * 3 * 4096 + 22), &pool);
        PricingSettings s;
        s.mc_paths = 4 * 1003;
        s.mc_rqmc_batches = 4;
        s.mc_sampler = SamplerKind::Sobol;
        expect_batch_equals_paths(Dyn::LocalVol, s, &pool);
    }

    // What does not batch: fuzzy scripts (fuzzy ifs), and AAD.
    TEST(ScriptBatch, FuzzyScriptsDoNotBatch)
    {
        ScriptSettings fuzzy;
        fuzzy.fuzzy = true;
        const ScriptedProduct<Real> f("2027-06-01\n    if spot() > 100 then pays 1 endIf\n", kCtx, fuzzy);
        EXPECT_EQ(f.batch_width(), 0);
        const ScriptedProduct<Real> h("2027-06-01\n    if spot() > 100 then pays 1 endIf\n", kCtx);
        EXPECT_GT(h.batch_width(), 0);
    }

} // namespace quantModeling
