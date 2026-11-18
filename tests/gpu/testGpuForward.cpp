#include <gtest/gtest.h>

#include "../support/cpu_gpu_tolerance.hpp"

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"

#include "library_scripts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

// fwd(DATE) on the GPU (WP 16 §15, issue #315): the IR binds it to the path
// as spot(0) e^((r - q) tau), tau in the date's data -- the CPU models'
// forward. Price and adjoint risks against the CPU (the bytecode, and the
// tape), within the project's CPU/GPU tolerances, under every model the
// kernels simulate; the rate and the dividend now reach the risks through the
// forward.

namespace quantModeling
{
    namespace
    {
        using namespace gpu_test;
        using test_support::price_tolerance;
        using test_support::risk_tolerance;

        /// A forward-start on the forward, a forward held to its date, and a
        /// fixed-rate accrual: fwd(), df(), yf() and fwdRate() in one script.
        const std::string kScript = "2026-09-01\n    s1 = spot()\n"
                                    "2027-03-01\n"
                                    "    pays max(fwd(2027-09-01) - s1, 0) * df(2027-09-01)\n"
                                    "    pays 0.5 * fwd(2027-06-01) * df(2027-06-01)\n"
                                    "    pays 100 * fwdRate(2027-03-01, 2027-09-01) * yf(2027-03-01, 2027-09-01)\n";

        std::string script(std::size_t n_assets)
        {
            // a multi-asset script reads its other spots; fwd() is spot(0)'s
            return n_assets > 1 ? kScript + "    pays 0 * spot(1)\n" : kScript;
        }

        PricingSettings settings(ComputeDevice device, SamplerKind sampler)
        {
            PricingSettings s;
            s.mc_paths = 8192;
            s.mc_seed = 41;
            s.mc_antithetic = true;
            s.mc_rng = RngKind::Philox;
            s.mc_sampler = sampler;
            s.mc_device = device;
            return s;
        }

        void expect_price_matches(Dyn dyn, std::size_t n_assets, SamplerKind sampler)
        {
            const ScriptedProduct<Real> product(script(n_assets), kCtx);
            ASSERT_FALSE(product.ir().empty()) << product.ir_refusal();
            ASSERT_TRUE(product.ir().has_forward);
            auto cpu_model = model(dyn, n_assets), gpu_model = model(dyn, n_assets);
            const SimulationMCResult cpu = simulate_script(product, *cpu_model, settings(ComputeDevice::Cpu, sampler));
            const SimulationMCResult gpu = simulate_script(product, *gpu_model, settings(ComputeDevice::Gpu, sampler));
            ASSERT_EQ(gpu.device, "gpu") << gpu.diagnostics;
            EXPECT_EQ(gpu.n_paths, cpu.n_paths);
            EXPECT_NEAR(gpu.npv(), cpu.npv(), price_tolerance(cpu.npv())) << gpu.diagnostics;
            EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-7 * std::max(1e-12, cpu.std_error()));
        }

        template <class Make>
        void expect_risks_match(Make make)
        {
            constexpr std::size_t kPaths = 4096; // a multiple of simulate_aad's batch of 64
            constexpr std::uint64_t kSeed = 23;
            const ScriptedProduct<Real> product(kScript, kCtx);
            auto model = make(Real{});
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << why;

            const ScriptedProduct<aad::Number> tape_product(kScript, kCtx);
            auto tape_model = make(aad::Number{});
            const AADSimulResults cpu =
                simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            EXPECT_NEAR(gpu->price, cpu.price, price_tolerance(cpu.price));
            ASSERT_EQ(gpu->risk_labels, cpu.risk_labels);
            double largest = 0.0;
            for (double x : cpu.risks)
                largest = std::max(largest, std::abs(x));
            bool saw_div = false;
            for (std::size_t i = 0; i < cpu.risks.size(); ++i)
            {
                EXPECT_NEAR(gpu->risks[i], cpu.risks[i], risk_tolerance(largest)) << cpu.risk_labels[i];
                // the dividend reaches the price through fwd() only
                if (cpu.risk_labels[i] == "div")
                    saw_div = std::abs(cpu.risks[i]) > 1.0;
            }
            EXPECT_TRUE(saw_div);
        }
    } // namespace

    class GpuForwardTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuForwardTest, PriceUnderBlackScholes)
    {
        expect_price_matches(Dyn::BlackScholes, 1, SamplerKind::PseudoRandom);
        expect_price_matches(Dyn::BlackScholes, 1, SamplerKind::Sobol);
    }
    TEST_F(GpuForwardTest, PriceUnderLocalVol)
    {
        expect_price_matches(Dyn::LocalVol, 1, SamplerKind::Sobol);
    }
    TEST_F(GpuForwardTest, PriceUnderHeston)
    {
        expect_price_matches(Dyn::Heston, 1, SamplerKind::Sobol);
    }
    TEST_F(GpuForwardTest, PriceUnderSlv)
    {
        expect_price_matches(Dyn::SLV, 1, SamplerKind::Sobol);
    }
    TEST_F(GpuForwardTest, PriceOnThreeAssets)
    {
        expect_price_matches(Dyn::BlackScholes, 3, SamplerKind::Sobol);
    }

    TEST_F(GpuForwardTest, AdjointRisksUnderBlackScholesMatchTheTape)
    {
        expect_risks_match([](auto zero)
                           {
            using T = decltype(zero);
            return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.02), T(0.25)); });
    }

    TEST_F(GpuForwardTest, AdjointRisksUnderLocalVolMatchTheTape)
    {
        expect_risks_match([](auto zero)
                           {
            using T = decltype(zero);
            std::vector<T> grid;
            for (Real v : skewed(0.22, 0.3))
                grid.emplace_back(v);
            return std::make_unique<LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.02), kK, kT, grid); });
    }

    TEST_F(GpuForwardTest, AdjointRisksUnderHestonMatchTheTape)
    {
        expect_risks_match([](auto zero)
                           {
            using T = decltype(zero);
            return std::make_unique<BatesSimModel<T>>(T(100.0), T(0.03), T(0.02), T(0.04), T(1.5), T(0.05),
                                                      T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0)); });
    }

    // With compilation off the interpreted kernels would not read the
    // forward: the request says so instead of pricing.
    TEST_F(GpuForwardTest, TheInterpreterRefusesFwd)
    {
        setenv("QM_SCRIPT_JIT", "0", 1);
        const ScriptedProduct<Real> product(kScript, kCtx);
        auto m = model(Dyn::BlackScholes, 1);
        EXPECT_THROW(simulate_script(product, *m, settings(ComputeDevice::Gpu, SamplerKind::Sobol)), InvalidInput);
        auto m2 = model(Dyn::BlackScholes, 1);
        const SimulationMCResult cpu = simulate_script(product, *m2, settings(ComputeDevice::Auto, SamplerKind::Sobol));
        EXPECT_NE(cpu.diagnostics.find("fwd()"), std::string::npos) << cpu.diagnostics;
        unsetenv("QM_SCRIPT_JIT");
    }

} // namespace quantModeling
