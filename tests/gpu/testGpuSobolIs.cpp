#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"

// Sobol RQMC with the Brownian bridge, and generic importance sampling, for
// scripts on the GPU (blueprint/wp/19-gpu.md §2.2, §2.5):
//   - the 42 library scripts in Sobol give the CPU generic engine's price and
//     error, under Black-Scholes, local vol, Heston and SLV;
//   - importance sampling takes the same decision on both sides (the pilot
//     runs the same draws) and gives the same numbers;
//   - model risks under Sobol equal the CPU tape's (duals, local-vol adjoint).

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

        template <class T>
        std::vector<T> skewed(double level, double skew)
        {
            std::vector<T> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(T(level + skew * (100.0 - k) / 100.0 + 0.005 * t));
            return g;
        }

        enum class Dyn
        {
            BlackScholes,
            LocalVol,
            Heston,
            SLV
        };

        std::unique_ptr<ISimulationModel<Real>> model(Dyn dyn)
        {
            switch (dyn)
            {
                case Dyn::LocalVol:
                    return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed<Real>(0.22, 0.3));
                case Dyn::Heston:
                    return std::make_unique<BatesSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0,
                                                                 0.0, 0.0);
                case Dyn::SLV:
                    return std::make_unique<SLVSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT,
                                                               skewed<Real>(1.0, 0.4));
                default:
                    return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
            }
        }

        class GpuSobolIsTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                if (gpu::device_count() == 0)
                    GTEST_SKIP() << "no CUDA device";
            }
        };

        void expect_same(const SimulationMCResult &gpu, const SimulationMCResult &cpu, const std::string &what)
        {
            ASSERT_EQ(gpu.device, "gpu") << what << ": " << gpu.diagnostics;
            EXPECT_EQ(gpu.n_paths, cpu.n_paths) << what;
            const double scale = std::max(1.0, std::abs(cpu.npv()));
            EXPECT_NEAR(gpu.npv(), cpu.npv(), 1e-9 * scale) << what << " " << gpu.diagnostics;
            EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-6 * std::max(1e-12, cpu.std_error()) + 1e-12)
                << what << " " << gpu.diagnostics;
        }

        void expect_sobol_gpu_equals_cpu(Dyn dyn, bool owen = false)
        {
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                if (product.n_underlyings() > 1)
                    continue;
                PricingSettings set;
                set.mc_paths = 8192;
                set.mc_seed = 9;
                set.mc_sampler = SamplerKind::Sobol;
                set.mc_sobol_owen = owen;
                auto cpu_model = model(dyn);
                auto gpu_model = model(dyn);
                set.mc_device = ComputeDevice::Cpu;
                const SimulationMCResult cpu = simulate_script(product, *cpu_model, set);
                set.mc_device = ComputeDevice::Gpu;
                const SimulationMCResult gpu = simulate_script(product, *gpu_model, set);
                expect_same(gpu, cpu, s.name);
                EXPECT_NE(gpu.diagnostics.find("Sobol RQMC"), std::string::npos) << gpu.diagnostics;
            }
        }
    } // namespace

    TEST_F(GpuSobolIsTest, SobolLibraryUnderBlackScholes)
    {
        expect_sobol_gpu_equals_cpu(Dyn::BlackScholes);
    }
    TEST_F(GpuSobolIsTest, SobolLibraryUnderLocalVol)
    {
        expect_sobol_gpu_equals_cpu(Dyn::LocalVol);
    }
    TEST_F(GpuSobolIsTest, SobolLibraryUnderHeston)
    {
        expect_sobol_gpu_equals_cpu(Dyn::Heston);
    }
    TEST_F(GpuSobolIsTest, SobolLibraryUnderSlv)
    {
        expect_sobol_gpu_equals_cpu(Dyn::SLV);
    }
    // Owen's scrambling: the same points on both sides (owen_scramble is
    // integer arithmetic, host and device).
    TEST_F(GpuSobolIsTest, OwenSobolLibraryUnderLocalVol)
    {
        expect_sobol_gpu_equals_cpu(Dyn::LocalVol, true);
    }
    TEST_F(GpuSobolIsTest, OwenSobolLibraryUnderHeston)
    {
        expect_sobol_gpu_equals_cpu(Dyn::Heston, true);
    }

    TEST_F(GpuSobolIsTest, ImportanceSamplingTakesTheCpusDecisionAndNumbers)
    {
        const std::vector<std::string> scripts{"2027-06-01\n    if spot() > 180 then pays 100 endIf\n",
                                               library().front().source};
        for (const std::string &src : scripts)
            for (const SamplerKind sampler :
                 {SamplerKind::PseudoRandom, SamplerKind::Stratified, SamplerKind::Sobol})
            {
                const ScriptedProduct<Real> product(src, kCtx);
                PricingSettings set;
                set.mc_paths = 1 << 15;
                set.mc_seed = 4;
                set.mc_rng = RngKind::Philox;
                set.mc_sampler = sampler;
                set.mc_importance_drift = true;
                auto cpu_model = model(Dyn::LocalVol);
                auto gpu_model = model(Dyn::LocalVol);
                set.mc_device = ComputeDevice::Cpu;
                const SimulationMCResult cpu = simulate_script(product, *cpu_model, set);
                set.mc_device = ComputeDevice::Gpu;
                const SimulationMCResult gpu = simulate_script(product, *gpu_model, set);
                expect_same(gpu, cpu, "importance sampling");
                const auto kept = [](const std::string &d)
                { return d.find("+ importance sampling (drift") != std::string::npos; };
                EXPECT_EQ(kept(gpu.diagnostics), kept(cpu.diagnostics)) << gpu.diagnostics << "\n"
                                                                        << cpu.diagnostics;
            }
    }

    // ── model risks under Sobol: GPU = tape ────────────────────────────────
    TEST_F(GpuSobolIsTest, RisksUnderSobolMatchTheTape)
    {
        const std::size_t n = 16 * 256; // kAadRqmcReplicates x 256
        for (const std::string name : {"european-call", "asian-call", "phoenix-autocall", "up-and-out-call"})
        {
            std::string src;
            for (const auto &s : library())
                if (s.name == name)
                    src = s.source;
            const ScriptedProduct<Real> product(src, kCtx);
            const ScriptedProduct<aad::Number> tape_product(src, kCtx);
            for (const bool local_vol : {true, false})
            {
                std::unique_ptr<ISimulationModel<Real>> m;
                std::unique_ptr<ISimulationModel<aad::Number>> tm;
                if (local_vol)
                {
                    m = std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed<Real>(0.22, 0.3),
                                                                 1.0 / 24.0);
                    tm = std::make_unique<LocalVolSimModel<aad::Number>>(
                        aad::Number(100.0), aad::Number(0.03), aad::Number(0.01), kK, kT,
                        skewed<aad::Number>(0.22, 0.3), 1.0 / 24.0);
                }
                else
                {
                    m = std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
                    tm = std::make_unique<BlackScholesSimModel<aad::Number>>(aad::Number(100.0), aad::Number(0.03),
                                                                             aad::Number(0.01), aad::Number(0.25));
                }
                std::string why;
                const auto gpu = simulate_script_aad_gpu(product, *m, n, 21, why, SamplerKind::Sobol);
                ASSERT_TRUE(gpu.has_value()) << why;
                const AADSimulResults cpu =
                    simulate_aad(tape_product, *tm, n, 21, first_aad_payoff, RngKind::Philox, SamplerKind::Sobol);
                EXPECT_NEAR(gpu->price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << name;
                EXPECT_NEAR(gpu->price_std_error, cpu.price_std_error, 1e-6 * cpu.price_std_error + 1e-12) << name;
                double scale = 1.0;
                for (Real r : cpu.risks)
                    scale = std::max(scale, std::abs(r));
                ASSERT_EQ(gpu->risks.size(), cpu.risks.size());
                for (std::size_t i = 0; i < cpu.risks.size(); ++i)
                {
                    EXPECT_NEAR(gpu->risks[i], cpu.risks[i], 1e-8 * scale) << name << " " << cpu.risk_labels[i];
                    EXPECT_NEAR(gpu->risk_std_errors[i], cpu.risk_std_errors[i],
                                1e-6 * cpu.risk_std_errors[i] + 1e-8 * scale)
                        << name << " " << cpu.risk_labels[i];
                }
            }
        }
    }

} // namespace quantModeling
