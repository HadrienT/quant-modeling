#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"

#include "library_scripts.hpp"

// Lot P1b of blueprint/wp/26-performance.md: a script's events compiled to
// CUDA at run time (NVRTC, linked into the kernel by nvJitLink) give the
// interpreter's result bit for bit -- every library script, crisp and fuzzy,
// on every GPU model, Philox and Sobol.

namespace quantModeling
{
    namespace
    {
        using namespace gpu_test;

        /// QM_SCRIPT_JIT for the scope: "0" runs the interpreter.
        struct JitSwitch
        {
            explicit JitSwitch(bool on) { setenv("QM_SCRIPT_JIT", on ? "1" : "0", 1); }
            ~JitSwitch() { unsetenv("QM_SCRIPT_JIT"); }
            JitSwitch(const JitSwitch &) = delete;
            JitSwitch &operator=(const JitSwitch &) = delete;
        };

        PricingSettings settings(SamplerKind sampler)
        {
            PricingSettings s;
            s.mc_paths = 8192;
            s.mc_seed = 17;
            s.mc_antithetic = true;
            s.mc_rng = RngKind::Philox;
            s.mc_sampler = sampler;
            s.mc_device = ComputeDevice::Gpu;
            return s;
        }

        bool same_bits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }

        void expect_compiled_equals_interpreted(Dyn dyn, bool fuzzy, bool single_asset_only, SamplerKind sampler)
        {
            int compared = 0;
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(s.source, kCtx, ss);
                if (single_asset_only && product.n_underlyings() > 1)
                    continue;
                auto m1 = model(dyn, product.n_underlyings());
                auto m2 = model(dyn, product.n_underlyings());
                SimulationMCResult interpreted, compiled;
                {
                    const JitSwitch off(false);
                    interpreted = simulate_script(product, *m1, settings(sampler));
                }
                {
                    const JitSwitch on(true);
                    compiled = simulate_script(product, *m2, settings(sampler));
                }
                const std::string what = s.name + (fuzzy ? " (fuzzy)" : "");
                ASSERT_EQ(compiled.device, "gpu") << what << ": " << compiled.diagnostics;
                EXPECT_NE(compiled.diagnostics.find("compiled script"), std::string::npos)
                    << what << ": " << compiled.diagnostics;
                EXPECT_NE(interpreted.diagnostics.find("interpreted bytecode"), std::string::npos)
                    << what << ": " << interpreted.diagnostics;
                EXPECT_TRUE(same_bits(compiled.npv(), interpreted.npv()))
                    << what << ": compiled " << compiled.npv() << ", interpreted " << interpreted.npv() << " (diff "
                    << compiled.npv() - interpreted.npv() << ")";
                EXPECT_TRUE(same_bits(compiled.std_error(), interpreted.std_error())) << what;
                ++compared;
            }
            EXPECT_GT(compared, 0);
        }
    } // namespace

    class GpuScriptJitTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuScriptJitTest, BlackScholesHardSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::BlackScholes, false, false, SamplerKind::PseudoRandom);
    }
    TEST_F(GpuScriptJitTest, BlackScholesFuzzySameBits)
    {
        expect_compiled_equals_interpreted(Dyn::BlackScholes, true, false, SamplerKind::PseudoRandom);
    }
    TEST_F(GpuScriptJitTest, BlackScholesSobolSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::BlackScholes, false, false, SamplerKind::Sobol);
    }
    TEST_F(GpuScriptJitTest, LocalVolSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::LocalVol, false, true, SamplerKind::PseudoRandom);
    }
    TEST_F(GpuScriptJitTest, LocalVolFuzzySobolSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::LocalVol, true, true, SamplerKind::Sobol);
    }
    TEST_F(GpuScriptJitTest, HestonSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::Heston, false, true, SamplerKind::PseudoRandom);
    }
    TEST_F(GpuScriptJitTest, SlvFuzzySameBits)
    {
        expect_compiled_equals_interpreted(Dyn::SLV, true, true, SamplerKind::PseudoRandom);
    }

    // Spot controls are not compiled yet: the interpreter runs, and says why.
    TEST_F(GpuScriptJitTest, SpotControlsStayOnTheInterpreter)
    {
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        auto m = model(Dyn::BlackScholes, product.n_underlyings());
        PricingSettings s = settings(SamplerKind::PseudoRandom);
        s.mc_spot_control = true;
        const SimulationMCResult r = simulate_script(product, *m, s);
        EXPECT_EQ(r.device, "gpu");
        EXPECT_NE(r.diagnostics.find("interpreted bytecode (spot controls"), std::string::npos) << r.diagnostics;
    }

} // namespace quantModeling
