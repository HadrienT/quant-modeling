#include <gtest/gtest.h>

#include "../support/cpu_gpu_tolerance.hpp"

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include "library_scripts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// WP 32 F4: the GPU's kernels on a forward curve -- each step's carry and
// the events' discounts, numeraires and forwards the CPU model's -- against
// the CPU within the project's CPU/GPU tolerance (WP 26: 1e-12 on prices),
// compiled from the IR and interpreted, under every model the device
// simulates. Risks on a curve stay on the CPU's tape.

namespace quantModeling
{
    namespace
    {
        using namespace gpu_test;
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;
        using test_support::price_tolerance;
        using test_support::risk_tolerance;

        // A rate rising from 4.4 % to 5.1 %, and a cash dividend of 1.5 in
        // the third month as two pillars a second apart (the C++ tests').
        ForwardCurve curve()
        {
            const double S0 = 100.0, tau = 91.0 / 365.0, gap = 1.0 / (365.0 * 86400.0);
            const std::vector<Time> T{2.0 / 12, tau, tau + gap, 0.5, 1.0, 2.0};
            const std::vector<Real> r{0.044, 0.045, 0.045, 0.047, 0.050, 0.051};
            std::vector<Real> F, DF;
            for (std::size_t i = 0; i < T.size(); ++i)
            {
                const double fwd = S0 * std::exp((r[i] - 0.012) * T[i]);
                F.push_back(T[i] > tau ? fwd - 1.5 * std::exp((r[i] - 0.012) * (T[i] - tau)) : fwd);
                DF.push_back(std::exp(-r[i] * T[i]));
            }
            return ForwardCurve::from_pillars(S0, T, F, DF);
        }

        ScriptModelSpec spec(const std::string &model, bool qe = false)
        {
            ScriptModelSpec s;
            // Heston and SLV price by QE on the site (#317): both schemes.
            s.variance_scheme = qe ? mc::VarianceScheme::QE : mc::VarianceScheme::Euler;
            s.model = model;
            s.spot = 100.0;
            s.vol = 0.2;
            s.max_dt = 1.0 / 52.0;
            s.heston = HestonParams{0.04, 1.5, 0.04, 0.5, -0.7};
            if (model == "local_vol" || model == "slv")
            {
                s.K_grid = {40.0, 80.0, 100.0, 120.0, 250.0};
                s.T_grid = {0.05, 0.5, 3.0};
                for (const double K : s.K_grid)
                    for (std::size_t j = 0; j < s.T_grid.size(); ++j)
                    {
                        const double skew = 0.2 - 0.08 * std::log(K / 100.0);
                        s.sigma_loc_flat.push_back(skew);
                        s.leverage_flat.push_back(skew / 0.2);
                    }
            }
            s.forward_curve = curve();
            return s;
        }

        // Dates on both sides of the pillars and of the dividend; spot(),
        // df() past the curve's last pillar, and a payment at each date.
        const std::string kScript = "2026-08-01\n    s1 = spot()\n    pays 0.1 * df(2028-12-01)\n\n"
                                    "2026-09-01\n    pays 0.2 * spot()\n\n"
                                    "2027-03-01\n    pays max(spot() - s1, 0)\n";
        // The same, and fwd(): the kernels compiled from the IR only.
        const std::string kWithForward = kScript + "    pays 0.5 * fwd(2027-09-01) * df(2027-09-01)\n";

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

        void expect_price_matches(const std::string &model, const std::string &src, SamplerKind sampler,
                                  bool qe = false)
        {
            const ScriptedProduct<Real> product(src, kCtx);
            auto cpu_model = make_script_model<Real>(spec(model, qe));
            auto gpu_model = make_script_model<Real>(spec(model, qe));
            const SimulationMCResult cpu = simulate_script(product, *cpu_model, settings(ComputeDevice::Cpu, sampler));
            const SimulationMCResult gpu = simulate_script(product, *gpu_model, settings(ComputeDevice::Gpu, sampler));
            ASSERT_EQ(gpu.device, "gpu") << model << ": " << gpu.diagnostics;
            EXPECT_EQ(gpu.n_paths, cpu.n_paths);
            EXPECT_NEAR(gpu.npv(), cpu.npv(), price_tolerance(cpu.npv())) << model << ": " << gpu.diagnostics;
        }

        const std::vector<std::string> kModels{"black_scholes", "local_vol", "heston", "slv"};
    } // namespace

    class GpuForwardCurveTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuForwardCurveTest, CompiledKernelsPriceOnTheCurve)
    {
        for (const std::string &m : kModels)
        {
            expect_price_matches(m, kWithForward, SamplerKind::PseudoRandom);
            expect_price_matches(m, kWithForward, SamplerKind::Sobol);
            if (m == "heston" || m == "slv")
                expect_price_matches(m, kWithForward, SamplerKind::Sobol, true);
        }
    }

    TEST_F(GpuForwardCurveTest, InterpretedKernelsPriceOnTheCurve)
    {
        setenv("QM_SCRIPT_JIT", "0", 1);
        for (const std::string &m : kModels)
        {
            expect_price_matches(m, kScript, SamplerKind::PseudoRandom);
            if (m == "heston" || m == "slv")
                expect_price_matches(m, kScript, SamplerKind::PseudoRandom, true);
        }
        unsetenv("QM_SCRIPT_JIT");
    }

    // The curve moves the GPU's price: the same model without it differs by
    // far more than the tolerance (the kernels do not ignore it).
    TEST_F(GpuForwardCurveTest, TheCurveReachesTheKernels)
    {
        const ScriptedProduct<Real> product(kWithForward, kCtx);
        ScriptModelSpec flat = spec("local_vol");
        flat.forward_curve.reset();
        flat.rate = 0.05;
        flat.dividend = 0.012;
        auto on = make_script_model<Real>(spec("local_vol")), off = make_script_model<Real>(flat);
        const SimulationMCResult a = simulate_script(product, *on, settings(ComputeDevice::Gpu, SamplerKind::Sobol));
        const SimulationMCResult b = simulate_script(product, *off, settings(ComputeDevice::Gpu, SamplerKind::Sobol));
        ASSERT_EQ(a.device, "gpu") << a.diagnostics;
        EXPECT_GT(std::abs(a.npv() - b.npv()), 1e3 * price_tolerance(a.npv()));
    }

    // Multilevel QMC on the GPU (local vol's default on the site): every
    // level's model on the curve; within error of the CPU's multilevel run.
    TEST_F(GpuForwardCurveTest, MultilevelOnTheCurveIsTheCpusWithinError)
    {
        const ScriptedProduct<Real> product(kScript, kCtx);
        auto m = make_script_model<Real>(spec("local_vol"));
        const double eps = 0.01;
        PricingSettings s;
        s.mc_mlmc_eps = eps;
        s.mc_seed = 37;
        s.mc_device = ComputeDevice::Gpu;
        const SimulationMCResult gpu = simulate_script(product, *m, s);
        s.mc_device = ComputeDevice::Cpu;
        const SimulationMCResult cpu = simulate_script(product, *m, s);
        EXPECT_EQ(gpu.device, "gpu") << gpu.diagnostics;
        EXPECT_NE(gpu.diagnostics.find("multilevel QMC"), std::string::npos) << gpu.diagnostics;
        EXPECT_NEAR(gpu.npv(), cpu.npv(), 3.0 * std::sqrt(2.0) * eps) << gpu.diagnostics << '\n'
                                                                      << cpu.diagnostics;
    }

    // The risks on a curve: the kernels compiled from the IR (the tangent
    // under Black-Scholes and Heston, the per-path adjoint under local vol
    // and SLV) against the CPU's tape on the same Philox paths, within the
    // project's tolerance (WP 26: 1e-10 of the largest risk). The tape adds
    // one risk per pillar, which sum to the rate and the dividend the
    // kernels take as parallel shifts; the GPU's labels are the tape's less
    // the pillars, and it says so.
    TEST_F(GpuForwardCurveTest, RisksOnTheCurveAreTheTapes)
    {
        constexpr std::size_t kPaths = 4096; // a multiple of simulate_aad's batch of 64
        constexpr std::uint64_t kSeed = 23;
        for (const std::string &m : kModels)
        {
            const ScriptedProduct<Real> product(kWithForward, kCtx);
            auto model = make_script_model<Real>(spec(m));
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << m << ": " << why;
            EXPECT_NE(gpu->diagnostics.find("forward curve"), std::string::npos) << gpu->diagnostics;

            const ScriptedProduct<aad::Number> tape_product(kWithForward, kCtx);
            auto tape_model = make_script_model<aad::Number>(spec(m));
            const AADSimulResults cpu =
                simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            EXPECT_NEAR(gpu->price, cpu.price, price_tolerance(cpu.price)) << m;

            double largest = 0.0, zero_sum = 0.0, yield_sum = 0.0;
            for (std::size_t i = 0; i < cpu.risks.size(); ++i)
            {
                largest = std::max(largest, std::abs(cpu.risks[i]));
                if (cpu.risk_labels[i].rfind("zero[", 0) == 0)
                    zero_sum += cpu.risks[i];
                if (cpu.risk_labels[i].rfind("div_yield[", 0) == 0)
                    yield_sum += cpu.risks[i];
            }
            ASSERT_LT(gpu->risk_labels.size(), cpu.risk_labels.size()) << m;
            for (std::size_t i = 0; i < gpu->risk_labels.size(); ++i)
            {
                ASSERT_EQ(gpu->risk_labels[i], cpu.risk_labels[i]) << m;
                EXPECT_NEAR(gpu->risks[i], cpu.risks[i], risk_tolerance(largest)) << m << " " << cpu.risk_labels[i];
            }
            const auto at = [&](const std::string &label)
            {
                return gpu->risks[static_cast<std::size_t>(
                    std::find(gpu->risk_labels.begin(), gpu->risk_labels.end(), label) - gpu->risk_labels.begin())];
            };
            EXPECT_NEAR(at("rate"), zero_sum, 1e-9 * largest) << m;
            EXPECT_NEAR(at("div"), yield_sum, 1e-9 * largest) << m;
        }
    }

} // namespace quantModeling
