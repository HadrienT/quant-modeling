#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/script.hpp"
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

        void expect_compiled_equals_interpreted(Dyn dyn, bool fuzzy, bool single_asset_only, SamplerKind sampler,
                                                bool terms = false)
        {
            int compared = 0;
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                ss.terms_as_parameters = terms; // the compiled kernel alone reads them: the oracle does not move
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

    // WP 27 lot L1b: the script's numbers read as terms, from data -- the
    // same bits as the interpreter, which holds them as constants.
    TEST_F(GpuScriptJitTest, TermsAsParametersBlackScholesSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::BlackScholes, false, false, SamplerKind::PseudoRandom, true);
    }
    TEST_F(GpuScriptJitTest, TermsAsParametersLocalVolFuzzySobolSameBits)
    {
        expect_compiled_equals_interpreted(Dyn::LocalVol, true, true, SamplerKind::Sobol, true);
    }

    // WP 27 lot L1b: a compiled kernel is stored on disk and read back by a
    // process that does not hold it -- the same bits, without compiling.
    TEST_F(GpuScriptJitTest, KernelsAreReadBackFromTheDiskCache)
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / ("qm-kernel-cache-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        setenv("QM_KERNEL_CACHE_DIR", dir.c_str(), 1);
        ASSERT_EQ(gpu::script_kernel_cache_dir(), dir.string());

        // A script no other test compiles (its constant is its own).
        const std::string script = "2027-06-01\n    pays max(spot() - 101.2345, 0)\n";
        const ScriptedProduct<Real> product(script, kCtx);
        const auto price = [&]()
        {
            auto m = model(Dyn::BlackScholes, 1);
            return simulate_script(product, *m, settings(SamplerKind::PseudoRandom));
        };
        const SimulationMCResult first = price();
        EXPECT_NE(first.diagnostics.find("compiled in"), std::string::npos) << first.diagnostics;
        std::size_t files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir))
            files += entry.path().extension() == ".qmk";
        EXPECT_EQ(files, 1u);

        EXPECT_NE(price().diagnostics.find("NVRTC, cached"), std::string::npos); // in memory

        gpu::forget_compiled_scripts(); // as a restart
        const SimulationMCResult again = price();
        EXPECT_NE(again.diagnostics.find("read from the kernel cache"), std::string::npos) << again.diagnostics;
        EXPECT_TRUE(same_bits(again.npv(), first.npv()));
        EXPECT_TRUE(same_bits(again.std_error(), first.std_error()));

        // A file that is not the kernel's is ignored: the script compiles.
        gpu::forget_compiled_scripts();
        for (const auto &entry : std::filesystem::directory_iterator(dir))
            std::filesystem::resize_file(entry.path(), 100);
        const SimulationMCResult rebuilt = price();
        EXPECT_NE(rebuilt.diagnostics.find("compiled in"), std::string::npos) << rebuilt.diagnostics;
        EXPECT_TRUE(same_bits(rebuilt.npv(), first.npv()));

        // A kernel linked with another build of the library (its header
        // names another embedded kernel) is stale: compiled again from the
        // source stored with it, and its file removed.
        gpu::forget_compiled_scripts();
        std::filesystem::path current;
        for (const auto &entry : std::filesystem::directory_iterator(dir))
            current = entry.path();
        const std::filesystem::path stale = dir / "0000000000000000-sm70.qmk";
        std::filesystem::rename(current, stale);
        {
            std::fstream f(stale, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(8); // the embedded kernel's hash
            const char other[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            f.write(other, sizeof other);
        }
        const gpu::ScriptKernelsRebuilt r = gpu::rebuild_script_kernels(0);
        EXPECT_EQ(r.rebuilt, 1);
        EXPECT_EQ(r.failed, 0);
        EXPECT_FALSE(std::filesystem::exists(stale));
        EXPECT_TRUE(std::filesystem::exists(current));
        EXPECT_EQ(gpu::rebuild_script_kernels(0).current, 1);
        gpu::forget_compiled_scripts();
        const SimulationMCResult warm = price();
        EXPECT_NE(warm.diagnostics.find("read from the kernel cache"), std::string::npos) << warm.diagnostics;
        EXPECT_TRUE(same_bits(warm.npv(), first.npv()));

        // Off: nothing is read or written.
        setenv("QM_KERNEL_CACHE", "0", 1);
        EXPECT_TRUE(gpu::script_kernel_cache_dir().empty());
        unsetenv("QM_KERNEL_CACHE");
        unsetenv("QM_KERNEL_CACHE_DIR");
        std::filesystem::remove_all(dir);
    }

    // Spot control variates run on the compiled kernel too (WP 27, lot L2;
    // #239's "compiled control variates"): the regression samples, hence
    // the controlled estimate and its error, are the interpreter's bits.
    TEST_F(GpuScriptJitTest, SpotControlsSameBits)
    {
        int compared = 0;
        for (const SamplerKind sampler : {SamplerKind::PseudoRandom, SamplerKind::Stratified})
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                auto m1 = model(Dyn::BlackScholes, product.n_underlyings());
                auto m2 = model(Dyn::BlackScholes, product.n_underlyings());
                PricingSettings ps = settings(sampler);
                ps.mc_spot_control = true;
                SimulationMCResult interpreted, compiled;
                {
                    const JitSwitch off(false);
                    interpreted = simulate_script(product, *m1, ps);
                }
                {
                    const JitSwitch on(true);
                    compiled = simulate_script(product, *m2, ps);
                }
                if (compiled.diagnostics.find("spot control") == std::string::npos)
                    continue; // no control was kept for this script
                EXPECT_NE(compiled.diagnostics.find("compiled script"), std::string::npos)
                    << s.name << ": " << compiled.diagnostics;
                EXPECT_NE(interpreted.diagnostics.find("interpreted bytecode"), std::string::npos)
                    << s.name << ": " << interpreted.diagnostics;
                EXPECT_TRUE(same_bits(compiled.npv(), interpreted.npv()))
                    << s.name << ": compiled " << compiled.npv() << ", interpreted " << interpreted.npv();
                EXPECT_TRUE(same_bits(compiled.std_error(), interpreted.std_error())) << s.name;
                ++compared;
            }
        EXPECT_GT(compared, 20);
    }

} // namespace quantModeling
