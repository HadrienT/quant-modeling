#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"

// Lot G2 of blueprint/wp/19-gpu.md: the 42 library scripts priced on the GPU
// give the CPU's price -- same units, same Philox draws, same reduction tree
// -- up to the last ulps of exp and sqrt.

namespace quantModeling
{
    namespace
    {
        struct LibraryScript
        {
            std::string name, source;
        };

        std::vector<LibraryScript> library()
        {
            std::vector<LibraryScript> out;
            for (const auto &entry : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
            {
                if (entry.path().extension() != ".qms")
                    continue;
                std::ifstream in(entry.path());
                std::stringstream text;
                text << in.rdbuf();
                out.push_back({entry.path().stem().string(), text.str()});
            }
            std::sort(out.begin(), out.end(), [](const auto &a, const auto &b)
                      { return a.name < b.name; });
            return out;
        }

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

        std::vector<Real> skewed(double level, double skew)
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(level + skew * (100.0 - k) / 100.0 + 0.005 * t);
            return g;
        }

        enum class Dyn
        {
            BlackScholes,
            LocalVol,
            Heston,
            SLV
        };

        std::unique_ptr<ISimulationModel<Real>> model(Dyn dyn, std::size_t n_assets)
        {
            if (n_assets > 1)
            {
                Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n_assets),
                                                                 static_cast<Eigen::Index>(n_assets), 0.4);
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
                    return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed(0.22, 0.3));
                case Dyn::Heston:
                    return std::make_unique<BatesSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0,
                                                                 0.0, 0.0);
                case Dyn::SLV:
                    return std::make_unique<SLVSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT,
                                                               skewed(1.0, 0.4));
                default:
                    return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
            }
        }

        PricingSettings settings(ComputeDevice device)
        {
            PricingSettings s;
            s.mc_paths = 8192;
            s.mc_seed = 31;
            s.mc_antithetic = true;
            s.mc_rng = RngKind::Philox;
            s.mc_device = device;
            return s;
        }

        void expect_gpu_equals_cpu(Dyn dyn, bool fuzzy, bool single_asset_only)
        {
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(s.source, kCtx, ss);
                if (single_asset_only && product.n_underlyings() > 1)
                    continue;
                auto cpu_model = model(dyn, product.n_underlyings());
                auto gpu_model = model(dyn, product.n_underlyings());
                const SimulationMCResult cpu = simulate_script(product, *cpu_model, settings(ComputeDevice::Cpu));
                const SimulationMCResult gpu = simulate_script(product, *gpu_model, settings(ComputeDevice::Gpu));
                ASSERT_EQ(gpu.device, "gpu") << s.name << ": " << gpu.diagnostics;
                EXPECT_EQ(gpu.n_paths, cpu.n_paths) << s.name;
                const double scale = std::max(1.0, std::abs(cpu.npv()));
                EXPECT_NEAR(gpu.npv(), cpu.npv(), 1e-9 * scale) << s.name << (fuzzy ? " fuzzy" : "");
                EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-7 * std::max(1e-12, cpu.std_error())) << s.name;
            }
        }
    } // namespace

    class GpuScriptsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuScriptsTest, LibraryUnderBlackScholesHard)
    {
        expect_gpu_equals_cpu(Dyn::BlackScholes, false, false);
    }
    TEST_F(GpuScriptsTest, LibraryUnderBlackScholesFuzzy)
    {
        expect_gpu_equals_cpu(Dyn::BlackScholes, true, false);
    }
    TEST_F(GpuScriptsTest, LibraryUnderLocalVol)
    {
        expect_gpu_equals_cpu(Dyn::LocalVol, false, true);
    }
    TEST_F(GpuScriptsTest, LibraryUnderHeston)
    {
        expect_gpu_equals_cpu(Dyn::Heston, false, true);
    }
    TEST_F(GpuScriptsTest, LibraryUnderSlv)
    {
        expect_gpu_equals_cpu(Dyn::SLV, true, true);
    }

    // What the GPU does not run: Auto falls back and says why, Gpu refuses.
    TEST_F(GpuScriptsTest, UnsupportedRequestsFallBackOrAreRefused)
    {
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        BatesSimModel<Real> jumps(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.3, -0.1, 0.2);

        PricingSettings s = settings(ComputeDevice::Auto);
        const SimulationMCResult r = simulate_script(product, jumps, s);
        EXPECT_EQ(r.device, "cpu");
        EXPECT_NE(r.diagnostics.find("runs on the CPU only"), std::string::npos) << r.diagnostics;

        s.mc_device = ComputeDevice::Gpu;
        EXPECT_THROW(simulate_script(product, jumps, s), InvalidInput);

        BlackScholesSimModel<Real> bs(100.0, 0.03, 0.01, 0.25);
        s.mc_sampler = SamplerKind::Sobol;
        EXPECT_THROW(simulate_script(product, bs, s), InvalidInput);
    }

} // namespace quantModeling
