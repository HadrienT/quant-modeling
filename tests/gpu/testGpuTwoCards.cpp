#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/sobol.hpp"

// Lot G4 of blueprint/wp/19-gpu.md §7: one GPU = two GPUs, bit for bit.
// The logical blocks are shared between the cards, every block reduced by the
// same tree, the partials folded on the host in block order -- so the
// number, not just the estimate, is independent of the card count. Checked on
// every engine the GPU runs: vanilla (Philox and Sobol), scripts (Philox,
// controls, stratified, Sobol, importance sampling; four models), dual and
// adjoint risks.

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

        std::unique_ptr<ISimulationModel<Real>> model(const std::string &kind, std::size_t n_assets)
        {
            if (n_assets > 1)
            {
                Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n_assets),
                                                                 static_cast<Eigen::Index>(n_assets), 0.4);
                corr.diagonal().setOnes();
                return std::make_unique<MultiAssetBSSimModel<Real>>(std::vector<Real>(n_assets, 100.0), 0.03,
                                                                    std::vector<Real>(n_assets, 0.01),
                                                                    std::vector<Real>(n_assets, 0.25), corr);
            }
            if (kind == "local_vol")
                return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed());
            if (kind == "heston")
                return std::make_unique<BatesSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0, 0.0,
                                                             0.0);
            return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
        }

        class GpuTwoCardsTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                if (gpu::device_count() < 2)
                    GTEST_SKIP() << "needs two CUDA devices";
            }
        };
    } // namespace

    TEST_F(GpuTwoCardsTest, VanillaIsTheSameBitsOnOneOrTwoCards)
    {
        gpu::VanillaGpuRequest req;
        req.spec.S0 = req.spec.K = 100.0;
        req.spec.sigma = 0.2;
        req.spec.T = 1.0;
        req.spec.sqrtT = 1.0;
        req.spec.movedSpot = 100.0 * std::exp(0.03 - 0.02);
        req.spec.rootVariance = 0.2;
        req.spec.df = std::exp(-0.05);
        req.spec.dS = 1.0;
        req.spec.factor_up = 1.01;
        req.spec.factor_dn = 0.99;
        req.spec.theta_bump = 1.0 / 365.0;
        req.spec.movedSpot_upT = req.spec.movedSpot_dnT = req.spec.movedSpot;
        req.spec.rootVariance_upT = req.spec.rootVariance_dnT = 0.2;
        req.spec.df_upT = req.spec.df_dnT = req.spec.df;
        req.n_units = 3'000'001; // an odd count: a partial last block
        req.seed = 5;
        req.devices = {0};
        const mc::VanillaStats one = gpu::simulate_vanilla_terminal(req);
        req.devices = {0, 1};
        const mc::VanillaStats two = gpu::simulate_vanilla_terminal(req);
        req.devices = {1, 0};
        const mc::VanillaStats swapped = gpu::simulate_vanilla_terminal(req);
        EXPECT_EQ(one.payoff.mean, two.payoff.mean);
        EXPECT_EQ(one.payoff.m2, two.payoff.m2);
        EXPECT_EQ(one.delta.mean, two.delta.mean);
        EXPECT_EQ(one.payoff.mean, swapped.payoff.mean);

        const SobolSequence seq(1, 77);
        gpu::VanillaSobolGpuRequest sob;
        sob.spec = req.spec;
        std::copy_n(seq.directions().begin(), 32, sob.directions);
        sob.shift = seq.shifts()[0];
        sob.n_points = 1'000'000;
        sob.devices = {0};
        const mc::VanillaStats s1 = gpu::simulate_vanilla_sobol(sob);
        sob.devices = {0, 1};
        const mc::VanillaStats s2 = gpu::simulate_vanilla_sobol(sob);
        EXPECT_EQ(s1.payoff.mean, s2.payoff.mean);
        EXPECT_EQ(s1.payoff.m2, s2.payoff.m2);
    }

    TEST_F(GpuTwoCardsTest, ScriptsAreTheSameBitsOnOneOrTwoCards)
    {
        struct Case
        {
            std::string name, kind;
            SamplerKind sampler;
            bool control, importance;
        };
        const Case cases[] = {
            {"phoenix-autocall", "local_vol", SamplerKind::PseudoRandom, false, false},
            {"asian-call", "local_vol", SamplerKind::PseudoRandom, true, false},
            {"up-and-out-call", "heston", SamplerKind::Stratified, false, false},
            {"up-and-out-call", "local_vol", SamplerKind::Stratified, true, false},
            {"variance-swap", "local_vol", SamplerKind::Sobol, false, false},
            {"asian-call", "heston", SamplerKind::Sobol, true, false},
            {"digital-call", "black_scholes", SamplerKind::PseudoRandom, false, true},
            {"worst-of-autocall", "black_scholes", SamplerKind::PseudoRandom, false, false},
        };
        for (const Case &c : cases)
        {
            const ScriptedProduct<Real> product(script(c.name), kCtx);
            PricingSettings s;
            s.mc_paths = 2'200'000; // >= 2 x kMinBlocksPerDevice logical blocks
            s.mc_seed = 12;
            s.mc_device = ComputeDevice::Gpu;
            s.mc_sampler = c.sampler;
            s.mc_spot_control = c.control;
            s.mc_importance_drift = c.importance;
            auto m1 = model(c.kind, product.n_underlyings());
            auto m2 = model(c.kind, product.n_underlyings());
            s.mc_gpus = 1;
            const SimulationMCResult one = simulate_script(product, *m1, s);
            s.mc_gpus = 2;
            const SimulationMCResult two = simulate_script(product, *m2, s);
            const std::string what = c.name + " " + c.kind + ": " + two.diagnostics;
            EXPECT_EQ(one.gpus, 1) << what;
            EXPECT_EQ(two.gpus, 2) << what;
            EXPECT_NE(two.diagnostics.find("2 x "), std::string::npos) << what;
            EXPECT_EQ(one.npv(), two.npv()) << what;
            EXPECT_EQ(one.std_error(), two.std_error()) << what;
        }
    }

    TEST_F(GpuTwoCardsTest, RisksAreTheSameBitsOnOneOrTwoCards)
    {
        for (const SamplerKind sampler : {SamplerKind::PseudoRandom, SamplerKind::Sobol})
            for (const std::string kind : {"local_vol", "heston", "black_scholes"})
            {
                const ScriptedProduct<Real> product(script("phoenix-autocall"), kCtx);
                auto m1 = model(kind, 1);
                auto m2 = model(kind, 1);
                std::string why;
                const std::size_t n = 1'100'000; // >= 2 x kMinBlocksPerDevice logical blocks
                const auto one = simulate_script_aad_gpu(product, *m1, n, 3, why, sampler, 1);
                ASSERT_TRUE(one.has_value()) << why;
                const auto two = simulate_script_aad_gpu(product, *m2, n, 3, why, sampler, 2);
                ASSERT_TRUE(two.has_value()) << why;
                EXPECT_EQ(one->gpus, 1);
                EXPECT_EQ(two->gpus, 2);
                EXPECT_EQ(one->price, two->price) << kind;
                EXPECT_EQ(one->price_std_error, two->price_std_error) << kind;
                ASSERT_EQ(one->risks.size(), two->risks.size());
                for (std::size_t i = 0; i < one->risks.size(); ++i)
                {
                    ASSERT_EQ(one->risks[i], two->risks[i]) << kind << " " << one->risk_labels[i];
                    ASSERT_EQ(one->risk_std_errors[i], two->risk_std_errors[i]) << kind << " " << one->risk_labels[i];
                }
            }
    }

    // A small run stays on one card (a second would cost more than it
    // saves) -- and says so.
    TEST_F(GpuTwoCardsTest, SmallRunsStayOnOneCard)
    {
        const ScriptedProduct<Real> product(script("european-call"), kCtx);
        PricingSettings s;
        s.mc_paths = 50'000;
        s.mc_device = ComputeDevice::Gpu;
        auto m = model("black_scholes", 1);
        const SimulationMCResult r = simulate_script(product, *m, s);
        EXPECT_EQ(r.gpus, 1) << r.diagnostics;
        EXPECT_EQ(r.diagnostics.find("2 x "), std::string::npos) << r.diagnostics;
    }

} // namespace quantModeling
