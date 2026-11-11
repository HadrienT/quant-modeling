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

#include "library_scripts.hpp"

// Lot G2 of blueprint/wp/19-gpu.md: the 42 library scripts priced on the GPU
// give the CPU's price -- same units, same Philox draws, same reduction tree
// -- up to the last ulps of exp and sqrt.

namespace quantModeling
{
    namespace
    {
        using namespace gpu_test;

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

        // Sobol, refused here until the GPU got its points and bridge (issue
        // #102), now runs on the device.
        BlackScholesSimModel<Real> bs(100.0, 0.03, 0.01, 0.25);
        s.mc_sampler = SamplerKind::Sobol;
        EXPECT_EQ(simulate_script(product, bs, s).device, "gpu");
    }

} // namespace quantModeling
