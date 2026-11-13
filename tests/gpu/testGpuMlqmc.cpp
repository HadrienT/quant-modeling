#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

// WP 26 P6 (blueprint/wp/26-performance.md §10): multilevel QMC on the GPU.
// Each level's fine and coarse grids are two Sobol runs of the script's
// kernel on the same points, coupled by the tables alone
// (mc::coupled_sobol_tables, tested on the host in tests/testMlmc.cpp).

namespace quantModeling
{
    namespace
    {
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;

        const ValuationContext kCtx{Date::from_iso("2026-01-01")};
        const std::string kCall = "2027-01-01\n    pays max(spot() - 95, 0)\n";
        // Twelve monthly fixings: state carried across the dates.
        const std::string kAsian = []
        {
            std::string s = "2026-02-01\n    sum = spot()\n";
            for (int m = 3; m <= 12; ++m)
                s += std::string("2026-") + (m < 10 ? "0" : "") + std::to_string(m) + "-01\n    sum = sum + spot()\n";
            return s + "2027-01-01\n    sum = sum + spot()\n    pays max(sum / 12 - 95, 0)\n";
        }();

        // tests/testMlmc.cpp's surfaces: monthly log-Euler steps on level 0.
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

        PricingSettings multilevel(double eps, ComputeDevice device)
        {
            PricingSettings s;
            s.mc_mlmc_eps = eps;
            s.mc_seed = 37;
            s.mc_device = device;
            return s;
        }

        class GpuMlqmcTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                if (gpu::device_count() < 1)
                    GTEST_SKIP() << "needs a CUDA device";
            }
        };
    } // namespace

    TEST_F(GpuMlqmcTest, AFlatSurfaceHasNothingToCorrect)
    {
        // Log-Euler is exact under a flat vol: fine and coarse paths end on
        // the same spot when they share their Brownian path, so every
        // correction is zero to rounding. Two runs on unrelated points
        // would leave the corrections at the size of their noise.
        ScriptedProduct<Real> product(kCall, kCtx);
        auto m = make_script_model<Real>(local_vol(true));
        const SimulationMCResult r = simulate_script(product, *m, multilevel(0.02, ComputeDevice::Gpu));
        EXPECT_EQ(r.device, "gpu");
        EXPECT_NE(r.diagnostics.find("multilevel QMC"), std::string::npos) << r.diagnostics;
        ASSERT_TRUE(r.discretisation_bias.has_value());
        EXPECT_LT(*r.discretisation_bias, 1e-10) << r.diagnostics;
    }

    TEST_F(GpuMlqmcTest, TheGpuRunIsTheCpuRunWithinError)
    {
        for (const std::string &script : {kCall, kAsian})
        {
            ScriptedProduct<Real> product(script, kCtx);
            auto m = make_script_model<Real>(local_vol(false));
            const double eps = 0.01;
            const SimulationMCResult gpu = simulate_script(product, *m, multilevel(eps, ComputeDevice::Gpu));
            const SimulationMCResult cpu = simulate_script(product, *m, multilevel(eps, ComputeDevice::Cpu));
            EXPECT_EQ(gpu.device, "gpu");
            EXPECT_EQ(cpu.device, "cpu");
            // Each within eps (RMS) of the same limit.
            EXPECT_NEAR(gpu.npv(), cpu.npv(), 3.0 * std::sqrt(2.0) * eps) << gpu.diagnostics << '\n'
                                                                          << cpu.diagnostics;
            EXPECT_LE(gpu.std_error(), 1.02 * eps / std::sqrt(2.0)) << gpu.diagnostics;
            ASSERT_TRUE(gpu.discretisation_bias.has_value());
            EXPECT_LE(*gpu.discretisation_bias, eps / std::sqrt(2.0) * 1.0001) << gpu.diagnostics;
        }
    }

    TEST_F(GpuMlqmcTest, TheSameBitsOnOneOrTwoCards)
    {
        if (gpu::device_count() < 2)
            GTEST_SKIP() << "needs two CUDA devices";
        ScriptedProduct<Real> product(kCall, kCtx);
        auto m = make_script_model<Real>(local_vol(false));
        PricingSettings s = multilevel(0.001, ComputeDevice::Gpu);
        s.mc_gpus = 1;
        const SimulationMCResult one = simulate_script(product, *m, s);
        s.mc_gpus = 2;
        const SimulationMCResult two = simulate_script(product, *m, s);
        EXPECT_EQ(one.gpus, 1);
        EXPECT_EQ(two.gpus, 2) << two.diagnostics; // enough blocks for the second card to join
        EXPECT_EQ(one.npv(), two.npv());
        EXPECT_EQ(one.std_error(), two.std_error());
        EXPECT_EQ(*one.discretisation_bias, *two.discretisation_bias);
    }

    TEST_F(GpuMlqmcTest, WhatTheDeviceCannotStepStaysOnTheCpu)
    {
        // Milstein is CPU only: asked for, the GPU refuses; on "auto" the
        // run stays on the CPU and says why.
        ScriptedProduct<Real> product(kCall, kCtx);
        ScriptModelSpec spec = local_vol(false);
        spec.milstein = true;
        auto m = make_script_model<Real>(spec);
        EXPECT_THROW(simulate_script(product, *m, multilevel(0.05, ComputeDevice::Gpu)), InvalidInput);
        const SimulationMCResult r = simulate_script(product, *m, multilevel(0.05, ComputeDevice::Auto));
        EXPECT_EQ(r.device, "cpu");
        EXPECT_NE(r.diagnostics.find("not on the GPU"), std::string::npos) << r.diagnostics;
    }

} // namespace quantModeling
