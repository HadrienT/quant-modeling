#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/models/device_model.hpp"
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

    // ── §10.2: the risks of a multilevel price ──────────────────────────

    TEST_F(GpuMlqmcTest, MultilevelRisksOnAFlatSurfaceAreBlackScholes)
    {
        // Flat local vol is Black-Scholes: the delta, and the sum of the
        // sigma_loc risks (a parallel move of the surface), are its delta
        // and vega.
        ScriptedProduct<Real> product(kCall, kCtx);
        auto m = make_script_model<Real>(local_vol(true));
        std::string why;
        const auto r = simulate_script_aad_mlqmc_gpu(product, *m, 0.01, 0.0, 37, why);
        ASSERT_TRUE(r.has_value()) << why;
        ASSERT_TRUE(r->discretisation_bias.has_value());
        EXPECT_LT(*r->discretisation_bias, 1e-10) << r->diagnostics;

        const double S = 100.0, K = 95.0, rate = 0.05, q = 0.02, vol = 0.25, T = 1.0;
        const double d1 = (std::log(S / K) + (rate - q + 0.5 * vol * vol) * T) / (vol * std::sqrt(T));
        const double cdf = 0.5 * std::erfc(-d1 / std::sqrt(2.0));
        const double pdf = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * std::acos(-1.0));
        const double delta = std::exp(-q * T) * cdf, vega = S * std::exp(-q * T) * pdf * std::sqrt(T);

        ASSERT_EQ(r->risk_labels.size(), r->risks.size());
        ASSERT_EQ(r->risk_labels.front(), "spot");
        EXPECT_NEAR(r->risks[0], delta, 4.0 * r->risk_std_errors[0] + 1e-4) << r->diagnostics;
        double parallel = 0.0;
        for (std::size_t i = 3; i < r->risks.size(); ++i)
            parallel += r->risks[i];
        EXPECT_NEAR(parallel, vega, 0.01 * vega) << r->diagnostics;
    }

    TEST_F(GpuMlqmcTest, MultilevelRisksAreTheMultilevelPricesSlope)
    {
        // Skewed surface: the adjoint delta against a central difference of
        // the multilevel price on the same points (same seed, a fixed
        // absolute target so that both bumps take the same levels).
        ScriptedProduct<Real> product(kCall, kCtx);
        const double eps = 0.005, h = 0.5;
        std::string why;
        auto price = [&](double spot)
        {
            ScriptModelSpec spec = local_vol(false);
            spec.spot = spot;
            auto m = make_script_model<Real>(spec);
            return simulate_script(product, *m, multilevel(eps, ComputeDevice::Gpu));
        };
        auto m = make_script_model<Real>(local_vol(false));
        const auto r = simulate_script_aad_mlqmc_gpu(product, *m, eps, 0.0, 37, why);
        ASSERT_TRUE(r.has_value()) << why;
        // The same estimator as the price route: the same number.
        const SimulationMCResult mid = price(100.0);
        EXPECT_NEAR(r->price, mid.npv(), 1e-9) << r->diagnostics << '\n'
                                               << mid.diagnostics;
        const SimulationMCResult up = price(100.0 + h), dn = price(100.0 - h);
        const double bump = (up.npv() - dn.npv()) / (2.0 * h);
        // Two prices each within eps (RMS): the difference quotient's noise.
        EXPECT_NEAR(r->risks[0], bump, 4.0 * (r->risk_std_errors[0] + std::sqrt(2.0) * eps / (2.0 * h)))
            << r->diagnostics;
        EXPECT_GT(r->risk_std_errors[0], 0.0);
        EXPECT_LT(r->risk_std_errors[0], 0.01); // a delta near 0.7, resolved
    }

    TEST_F(GpuMlqmcTest, MultilevelRisksSayWhyTheyCannotRun)
    {
        ScriptedProduct<Real> product(kCall, kCtx);
        ScriptModelSpec bs = local_vol(true);
        bs.model = "black_scholes";
        bs.vol = 0.2;
        auto m = make_script_model<Real>(bs);
        std::string why;
        EXPECT_FALSE(simulate_script_aad_mlqmc_gpu(product, *m, 0.01, 0.0, 1, why).has_value());
        EXPECT_NE(why.find("no time-step bias"), std::string::npos) << why;
    }

    // ── §10.3: a run over a range of points ─────────────────────────────

    TEST_F(GpuMlqmcTest, TwoHalvesOfASobolRunAreTheWholeRun)
    {
        ScriptedProduct<Real> product(kAsian, kCtx);
        auto m = make_script_model<Real>(local_vol(false));
        m->init(product.timeline(), product.defline());
        DeviceModel dm;
        ASSERT_TRUE(m->describe_device(dm));
        mc::SobolTables tables = mc::sobol_tables(m->sim_dim(), 7, 4, m->brownian_layout(), true);
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.ir = &product.ir();
        req.model = &dm;
        req.baseline = product.baseline();
        for (const SampleDef &def : product.defline())
            req.discount_mats.push_back(def.discount_mats);
        req.antithetic = false;
        req.sobol = &tables;
        auto means = [&](uint32_t first, uint64_t n)
        {
            tables.first_point = first;
            req.n_units = n;
            std::vector<double> out;
            for (const WelfordAccumulator &w : gpu::simulate_script(req).plain)
                out.push_back(w.mean);
            return out;
        };
        const uint64_t half = 5000; // not a block's multiple: partial blocks on both sides
        const std::vector<double> whole = means(0, 2 * half), a = means(0, half),
                                  b = means(static_cast<uint32_t>(half), half);
        ASSERT_EQ(whole.size(), 4u);
        for (std::size_t r = 0; r < whole.size(); ++r)
        {
            EXPECT_NEAR(0.5 * (a[r] + b[r]), whole[r], 1e-12 * std::fabs(whole[r])) << r;
            EXPECT_GT(std::fabs(a[r] - b[r]), 1e-6) << r; // other points, not the same ones twice
        }
    }

} // namespace quantModeling
