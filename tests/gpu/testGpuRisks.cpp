#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

#include "../library_exercise.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/superbucket.hpp"
#include "quantModeling/market/svi.hpp"
#include "quantModeling/market/svi_calibration.hpp"
#include "quantModeling/market/svi_surface.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/utils/accumulators.hpp"

#include "../ir_fixtures.hpp"
#include <chrono>
#include <cstdio>

// Lot G3 of blueprint/wp/19-gpu.md on the device:
//   - the generic variance reduction (spot controls, stratified terminal
//     value) gives the CPU's numbers -- same units, draws and estimators;
//   - the GPU's model risks equal the CPU tape's (simulate_aad, Philox):
//     duals under Black-Scholes, Heston and two assets, the per-path adjoint
//     under local vol, on the 42 library scripts;
//   - the adjoint is bit-reproducible whatever the launch size and card;
//   - the superbucket from GPU risks: the quote vegas add up to a parallel
//     shift of the quotes.

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
                if (library_script_has_exercise(text.str()))
                    continue;
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
        std::vector<T> skewed()
        {
            std::vector<T> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(T(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t));
            return g;
        }

        template <class T>
        std::unique_ptr<ISimulationModel<T>> local_vol()
        {
            return std::make_unique<LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.01), kK, kT, skewed<T>(), 1.0 / 24.0);
        }

        template <class T>
        std::unique_ptr<ISimulationModel<T>> multi_bs(std::size_t n)
        {
            Eigen::MatrixXd corr =
                Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n), 0.4);
            corr.diagonal().setOnes();
            std::vector<T> s0, q, vol;
            for (std::size_t i = 0; i < n; ++i)
            {
                s0.push_back(T(100.0 - 4.0 * static_cast<double>(i)));
                q.push_back(T(0.01));
                vol.push_back(T(0.2 + 0.04 * static_cast<double>(i)));
            }
            return std::make_unique<MultiAssetBSSimModel<T>>(s0, T(0.03), q, vol, corr);
        }

        class GpuRisksTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                if (gpu::device_count() == 0)
                    GTEST_SKIP() << "no CUDA device";
            }
        };

        void expect_same_risks(const AADSimulResults &gpu, const AADSimulResults &cpu, const std::string &what,
                               double rel)
        {
            ASSERT_EQ(gpu.risks.size(), cpu.risks.size()) << what;
            EXPECT_NEAR(gpu.price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << what;
            double scale = 1.0;
            for (Real r : cpu.risks)
                scale = std::max(scale, std::abs(r));
            for (std::size_t i = 0; i < cpu.risks.size(); ++i)
                EXPECT_NEAR(gpu.risks[i], cpu.risks[i], rel * scale) << what << " " << cpu.risk_labels[i];
        }

        constexpr std::size_t kPaths = 4096; // a multiple of simulate_aad's batch of 64
        constexpr std::uint64_t kSeed = 23;
    } // namespace

    // ── variance reduction: GPU = CPU ──────────────────────────────────────
    TEST_F(GpuRisksTest, VarianceReductionGivesTheCpuNumbers)
    {
        for (const SamplerKind sampler : {SamplerKind::PseudoRandom, SamplerKind::Stratified})
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                if (product.n_underlyings() > 1)
                    continue;
                PricingSettings set;
                set.mc_paths = 16384;
                set.mc_seed = 5;
                set.mc_rng = RngKind::Philox;
                set.mc_sampler = sampler;
                set.mc_spot_control = true;
                auto cpu_model = local_vol<Real>();
                auto gpu_model = local_vol<Real>();
                set.mc_device = ComputeDevice::Cpu;
                const SimulationMCResult cpu = simulate_script(product, *cpu_model, set);
                set.mc_device = ComputeDevice::Gpu;
                const SimulationMCResult gpu = simulate_script(product, *gpu_model, set);
                ASSERT_EQ(gpu.device, "gpu") << s.name << ": " << gpu.diagnostics;
                EXPECT_EQ(gpu.n_paths, cpu.n_paths) << s.name;
                const double scale = std::max(1.0, std::abs(cpu.npv()));
                EXPECT_NEAR(gpu.npv(), cpu.npv(), 1e-9 * scale) << s.name << " " << gpu.diagnostics;
                // A payoff the spot control absorbs entirely (the equity
                // forward, linear in the spot) leaves an error at the
                // round-off of the price: compared to the price's own
                // tolerance there, both are zero.
                EXPECT_NEAR(gpu.std_error(), cpu.std_error(), std::max(1e-6 * cpu.std_error(), 1e-9 * scale))
                    << s.name << " " << gpu.diagnostics;
            }
    }

    // ── duals: GPU = CPU tape ──────────────────────────────────────────────
    namespace
    {
        template <class Make>
        void expect_dual_risks_match(bool two_assets, Make make)
        {
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                const std::size_t n = product.n_underlyings();
                if (two_assets ? n != 2 : n != 1)
                    continue;
                auto model = make(Real{}, n);
                std::string why;
                const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
                ASSERT_TRUE(gpu.has_value()) << s.name << ": " << why;

                const ScriptedProduct<aad::Number> tape_product(s.source, kCtx);
                auto tape_model = make(aad::Number{}, n);
                const AADSimulResults cpu =
                    simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
                expect_same_risks(*gpu, cpu, s.name, 1e-9);
            }
        }
    } // namespace

    TEST_F(GpuRisksTest, DualsUnderBlackScholesMatchTheTape)
    {
        expect_dual_risks_match(false, [](auto zero, std::size_t)
                                {
            using T = decltype(zero);
            return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25)); });
    }

    TEST_F(GpuRisksTest, DualsUnderHestonMatchTheTape)
    {
        expect_dual_risks_match(false, [](auto zero, std::size_t)
                                {
            using T = decltype(zero);
            return std::make_unique<BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05),
                                                      T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0)); });
    }

    TEST_F(GpuRisksTest, DualsUnderTwoAssetsMatchTheTape)
    {
        expect_dual_risks_match(true, [](auto zero, std::size_t n)
                                {
            using T = decltype(zero);
            Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n),
                                                             static_cast<Eigen::Index>(n), 0.4);
            corr.diagonal().setOnes();
            std::vector<T> s0, q, vol;
            for (std::size_t i = 0; i < n; ++i)
            {
                s0.push_back(T(100.0 - 4.0 * static_cast<double>(i)));
                q.push_back(T(0.01));
                vol.push_back(T(0.2 + 0.04 * static_cast<double>(i)));
            }
            return std::make_unique<MultiAssetBSSimModel<T>>(s0, T(0.03), q, vol, corr); });
    }

    // ── per-path adjoint, local vol: GPU = CPU tape ────────────────────────
    TEST_F(GpuRisksTest, LocalVolAdjointMatchesTheTape)
    {
        for (const bool fuzzy : {false, true})
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(s.source, kCtx, ss);
                if (product.n_underlyings() > 1)
                    continue;
                auto model = local_vol<Real>();
                std::string why;
                const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
                ASSERT_TRUE(gpu.has_value()) << s.name << ": " << why;

                const ScriptedProduct<aad::Number> tape_product(s.source, kCtx, ss);
                auto tape_model = local_vol<aad::Number>();
                const AADSimulResults cpu =
                    simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
                expect_same_risks(*gpu, cpu, s.name + (fuzzy ? " fuzzy" : " hard"), 1e-8);
            }
    }

    // The risks' error bars (batches of 512 paths on the GPU, of 64 on the
    // tape) estimate the same thing: with 128 batches, within ~15 %.
    TEST_F(GpuRisksTest, AdjointErrorBarsMatchTheTapes)
    {
        const std::size_t n = 1u << 16;
        for (const std::string name : {"bonus-certificate", "asian-call", "athena-autocall"})
        {
            std::string source;
            for (const auto &s : library())
                if (s.name == name)
                    source = s.source;
            const ScriptedProduct<Real> product(source, kCtx);
            auto model = local_vol<Real>();
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, *model, n, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << why;
            const ScriptedProduct<aad::Number> tape_product(source, kCtx);
            auto tape_model = local_vol<aad::Number>();
            const AADSimulResults cpu =
                simulate_aad(tape_product, *tape_model, n, kSeed, first_aad_payoff, RngKind::Philox);
            for (std::size_t i = 0; i < 3; ++i)
                EXPECT_NEAR(gpu->risk_std_errors[i], cpu.risk_std_errors[i], 0.15 * cpu.risk_std_errors[i])
                    << name << " " << cpu.risk_labels[i];
        }
    }

    // Deterministic reduction: no atomics, fixed folding order -- the same
    // bits whatever the launch size and on either card.
    TEST_F(GpuRisksTest, AdjointIsBitReproducible)
    {
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        auto model = local_vol<Real>();
        model->init(product.timeline(), product.defline());
        DeviceModel dm;
        ASSERT_TRUE(model->describe_device(dm));
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.ir = &product.ir();
        req.model = &dm;
        req.baseline = product.baseline();
        for (const SampleDef &d : product.defline())
            req.discount_mats.push_back(d.discount_mats);
        req.antithetic = false;
        req.n_units = 40000;
        req.seed = 3;
        const auto a = gpu::simulate_script_adjoint(req);
        req.max_blocks_per_launch = 3;
        const auto b = gpu::simulate_script_adjoint(req);
        EXPECT_EQ(a.price.mean, b.price.mean);
        for (std::size_t i = 0; i < a.risks.size(); ++i)
            ASSERT_EQ(a.risks[i], b.risks[i]) << i;
        if (gpu::device_count() > 1)
        {
            req.device = 1;
            const auto c = gpu::simulate_script_adjoint(req);
            for (std::size_t i = 0; i < a.risks.size(); ++i)
                ASSERT_EQ(a.risks[i], c.risks[i]) << i;
        }
    }

    // ── the sparse gradient trail (issue #342) ─────────────────────────────
    namespace
    {
        // The page's grid: 100 strikes (40..200) by 129 dates over two years,
        // 12 900 nodes, of which a path's steps reach a few hundred.
        std::vector<Real> page_strikes()
        {
            std::vector<Real> k;
            for (int i = 0; i < 100; ++i)
                k.push_back(40.0 + 160.0 * i / 99.0);
            return k;
        }
        std::vector<Real> page_dates()
        {
            std::vector<Real> t;
            for (int j = 0; j < 129; ++j)
                t.push_back(2.0 * (j + 1) / 129.0);
            return t;
        }
        /// Local vol, or (slv) Heston times a leverage, on the page's grid.
        template <class T>
        std::unique_ptr<ISimulationModel<T>> page_model(bool slv)
        {
            std::vector<T> g;
            for (Real k : page_strikes())
                for (Real t : page_dates())
                    g.push_back(slv ? T(1.0 + 0.6 * (100.0 - k) / 100.0 + 0.02 * t)
                                    : T(0.2 + 0.25 * (100.0 - k) / 100.0 + 0.01 * t));
            if (!slv)
                return std::make_unique<LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.01), page_strikes(), page_dates(),
                                                             g, 1.0 / 252.0);
            return std::make_unique<SLVSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05), T(0.6),
                                                    T(-0.7), page_strikes(), page_dates(), g, 1.0 / 252.0);
        }
        std::string library_source(const std::string &name)
        {
            for (const auto &s : library())
                if (s.name == name)
                    return s.source;
            return {};
        }
    } // namespace

    // A two-year phoenix autocall stepped daily on the page's grid: each
    // path's trail holds hundreds of nodes (two a step under SLV, eight under
    // local vol), and the warps' paths collide on many of them. The fold
    // gives the tape's risks, every grid node included.
    TEST_F(GpuRisksTest, PageGridAdjointMatchesTheTape)
    {
        const std::string src = library_source("phoenix-autocall");
        ASSERT_FALSE(src.empty());
        for (const bool slv : {false, true})
        {
            const ScriptedProduct<Real> product(src, kCtx);
            auto model = page_model<Real>(slv);
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << why;
            EXPECT_NE(gpu->diagnostics.find("adjoint kernel"), std::string::npos) << gpu->diagnostics;
            const ScriptedProduct<aad::Number> tape_product(src, kCtx);
            auto tape_model = page_model<aad::Number>(slv);
            const AADSimulResults cpu =
                simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            ASSERT_GT(cpu.risks.size(), 12900u);
            expect_same_risks(*gpu, cpu, slv ? "slv" : "local vol", 1e-10);
            int reached = 0;
            for (Real r : gpu->risks)
                reached += r != 0.0 ? 1 : 0;
            EXPECT_GT(reached, 500) << "nodes with a risk";
        }
    }

    // The trail's fold has a fixed order: the same bits whatever the launch
    // size and on either card, under SLV on the page's grid.
    TEST_F(GpuRisksTest, PageGridSlvAdjointIsBitReproducible)
    {
        const ScriptedProduct<Real> product(library_source("phoenix-autocall"), kCtx);
        auto model = page_model<Real>(true);
        model->init(product.timeline(), product.defline());
        DeviceModel dm;
        ASSERT_TRUE(model->describe_device(dm));
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.ir = &product.ir();
        req.model = &dm;
        req.baseline = product.baseline();
        for (const SampleDef &d : product.defline())
            req.discount_mats.push_back(d.discount_mats);
        req.antithetic = false;
        req.n_units = 20000; // ten logical blocks, the last one partial
        req.seed = 3;
        const auto a = gpu::simulate_script_adjoint(req);
        req.max_blocks_per_launch = 3;
        const auto b = gpu::simulate_script_adjoint(req);
        EXPECT_EQ(a.price.mean, b.price.mean);
        ASSERT_EQ(a.risks.size(), b.risks.size());
        for (std::size_t i = 0; i < a.risks.size(); ++i)
        {
            ASSERT_EQ(a.risks[i], b.risks[i]) << i;
            ASSERT_EQ(a.risk_std_errors[i], b.risk_std_errors[i]) << i;
        }
        if (gpu::device_count() > 1)
        {
            req.device = 1;
            req.max_blocks_per_launch = 0;
            const auto c = gpu::simulate_script_adjoint(req);
            for (std::size_t i = 0; i < a.risks.size(); ++i)
                ASSERT_EQ(a.risks[i], c.risks[i]) << i;
        }
    }

    // Issue #348: the host folds the batches as they arrive, so an adjoint
    // run's memory does not grow with its paths (it held every batch's row,
    // ~385 B a path on the page's grid: 3.7 GB at 5M paths killed
    // production, #347). The peak RSS of 2M paths stays within a few MB of
    // 256k paths' (each TEST runs in its own process).
    TEST_F(GpuRisksTest, AdjointHostMemoryDoesNotGrowWithThePaths)
    {
        const ScriptedProduct<Real> product(library_source("phoenix-autocall"), kCtx);
        auto model = page_model<Real>(true);
        model->init(product.timeline(), product.defline());
        DeviceModel dm;
        ASSERT_TRUE(model->describe_device(dm));
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.ir = &product.ir();
        req.model = &dm;
        req.baseline = product.baseline();
        for (const SampleDef &d : product.defline())
            req.discount_mats.push_back(d.discount_mats);
        req.antithetic = false;
        req.seed = 5;
        const auto peak_mb = []
        {
            rusage u{};
            getrusage(RUSAGE_SELF, &u);
            return static_cast<double>(u.ru_maxrss) / 1024.0;
        };
        req.n_units = 1u << 18;
        ASSERT_FALSE(gpu::simulate_script_adjoint(req).risks.empty());
        const double small = peak_mb();
        req.n_units = 1u << 21;
        ASSERT_FALSE(gpu::simulate_script_adjoint(req).risks.empty());
        EXPECT_LT(peak_mb() - small, 64.0) << "2M paths against 256k, MB";
    }

    // Issue #350: a pool that keeps what a large request freed (the local-vol
    // adjoint's trails, ~4.7 GB for the phoenix on the page's grid) gives it
    // back to the card when the API calls trim_idle_pools once idle.
    TEST_F(GpuRisksTest, IdlePoolsGiveTheirMemoryBack)
    {
        // read once, at the pool's first use: this process's first CUDA call
        setenv("QM_GPU_POOL_RETAIN_MB", "8192", 1);
        const ScriptedProduct<Real> product(library_source("phoenix-autocall"), kCtx);
        auto model = page_model<Real>(false);
        model->init(product.timeline(), product.defline());
        DeviceModel dm;
        ASSERT_TRUE(model->describe_device(dm));
        gpu::ScriptGpuRequest req;
        req.program = &product.program();
        req.ir = &product.ir();
        req.model = &dm;
        req.baseline = product.baseline();
        for (const SampleDef &d : product.defline())
            req.discount_mats.push_back(d.discount_mats);
        req.antithetic = false;
        req.seed = 5;
        req.n_units = 1u << 16;
        const auto first = gpu::simulate_script_adjoint(req);
        const std::size_t free_before = gpu::free_memory(0);
        const std::size_t keep = std::size_t{256} << 20;
        const std::size_t released = gpu::trim_idle_pools(keep);
        EXPECT_GT(released, std::size_t{1} << 30) << "the trails' memory stayed in the pool";
        EXPECT_GT(gpu::free_memory(0), free_before + released / 2);
        EXPECT_EQ(gpu::trim_idle_pools(keep), 0u) << "nothing left to give back";
        // and the next request allocates again, to the same bits
        const auto again = gpu::simulate_script_adjoint(req);
        ASSERT_EQ(first.risks.size(), again.risks.size());
        for (std::size_t i = 0; i < first.risks.size(); ++i)
            ASSERT_EQ(first.risks[i], again.risks[i]) << i;
    }

    // ── the superbucket from GPU risks ─────────────────────────────────────
    namespace
    {
        constexpr Real kSpot = 100.0, kRate = 0.02, kDiv = 0.01;
        constexpr Real kKmin = -0.3, kKmax = 0.3;
        constexpr std::size_t kNK = 30, kNT = 12;
        const std::vector<Real> kTtms{0.25, 0.5, 1.0};

        SVIParams truth(Real T)
        {
            return SVIParams{0.03 * T, 0.12 * std::sqrt(T), -0.6, 0.02, 0.15};
        }

        std::vector<std::vector<SVISliceQuote>> quotes(Real bump_all = 0.0)
        {
            std::vector<std::vector<SVISliceQuote>> out;
            for (const Real T : kTtms)
            {
                std::vector<SVISliceQuote> q;
                for (int i = 0; i <= 14; ++i)
                {
                    const Real k = kKmin + (kKmax - kKmin) * i / 14.0;
                    q.push_back({k, svi_implied_vol(k, T, truth(T)) + bump_all, 1.0});
                }
                out.push_back(q);
            }
            return out;
        }

        std::vector<SVISliceCalibration> calibrate(const std::vector<std::vector<SVISliceQuote>> &q)
        {
            std::vector<SVISliceCalibration> out;
            for (std::size_t s = 0; s < kTtms.size(); ++s)
                out.push_back(calibrate_svi_slice(q[s], kTtms[s]));
            return out;
        }

        GridLocalVol grid_of(const std::vector<SVISliceCalibration> &slices)
        {
            return build_local_vol_grid(SVISurface(slices), kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT);
        }

        /// A 9-month call on the local vol from these slices, on the GPU.
        AADSimulResults gpu_call(const GridLocalVol &g)
        {
            const std::string script = "2027-03-01\n    pays max(spot() - 100, 0)\n";
            const ScriptedProduct<Real> product(script, kCtx);
            LocalVolSimModel<Real> m(kSpot, kRate, kDiv, g.K_grid(), g.T_grid(), g.sigma_loc(), 1.0 / 50.0);
            std::string why;
            const auto r = simulate_script_aad_gpu(product, m, 1u << 20, 11, why);
            if (!r)
                throw std::runtime_error(why);
            return *r;
        }
    } // namespace

    TEST_F(GpuRisksTest, SuperbucketQuoteVegasAddUpToAParallelShift)
    {
        const auto q = quotes();
        const auto slices = calibrate(q);
        const GridLocalVol g = grid_of(slices);
        const AADSimulResults r = gpu_call(g);
        const std::vector<Real> dV(r.risks.begin() + 3, r.risks.end()); // after spot, rate, div
        ASSERT_EQ(dV.size(), kNK * kNT);
        const SuperbucketResult b = dupire_superbucket(slices, q, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, dV);
        Real total = 0.0;
        for (const QuoteVega &v : b.quotes)
            total += v.vega;

        // Bump every quote, recalibrate, rebuild the grid, reprice on the GPU
        // with the same paths (common random numbers).
        const Real h = 1e-3;
        const Real up = gpu_call(grid_of(calibrate(quotes(h)))).price;
        const Real down = gpu_call(grid_of(calibrate(quotes(-h)))).price;
        const Real fd = (up - down) / (2 * h);
        EXPECT_NEAR(total, fd, 0.03 * std::fabs(fd)) << "adjoint " << total << " vs bump " << fd;
        EXPECT_GT(std::fabs(total), 1.0);
    }

    // ── WP 27, lots L3-L5: the risks generated from the IR ────────────────

    namespace
    {
        /// QM_SCRIPT_JIT for the scope: "0" runs the interpreter's duals and
        /// the adjoint written by hand.
        struct JitSwitch
        {
            explicit JitSwitch(bool on) { setenv("QM_SCRIPT_JIT", on ? "1" : "0", 1); }
            ~JitSwitch() { unsetenv("QM_SCRIPT_JIT"); }
            JitSwitch(const JitSwitch &) = delete;
            JitSwitch &operator=(const JitSwitch &) = delete;
        };

        void expect_same_risks(const AADSimulResults &a, const AADSimulResults &b, double rel, const std::string &what)
        {
            ASSERT_EQ(a.risks.size(), b.risks.size()) << what;
            double scale = 1e-12;
            for (Real r : b.risks)
                scale = std::max(scale, std::abs(r));
            for (std::size_t i = 0; i < a.risks.size(); ++i)
                EXPECT_NEAR(a.risks[i], b.risks[i], rel * scale) << what << " " << b.risk_labels[i];
        }
    } // namespace

    // The dual numbers on the interpreter and the local-vol adjoint written by
    // hand were deleted (2026-11-16) once the generated kernels beat them:
    // with compilation off the risks stay on the CPU tape and say why.
    TEST_F(GpuRisksTest, WithoutCompilationTheRisksStayOnTheCpu)
    {
        const JitSwitch off(false);
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        std::string why;
        BlackScholesSimModel<Real> bs(100.0, 0.03, 0.01, 0.25);
        EXPECT_FALSE(simulate_script_aad_gpu(product, bs, 4096, 5, why).has_value());
        EXPECT_NE(why.find("QM_SCRIPT_JIT=0"), std::string::npos) << why;
        LocalVolSimModel<Real> lv(100.0, 0.03, 0.01, kK, kT, skewed<Real>());
        why.clear();
        EXPECT_FALSE(simulate_script_aad_gpu(product, lv, 4096, 5, why).has_value());
        EXPECT_NE(why.find("QM_SCRIPT_JIT=0"), std::string::npos) << why;
    }

    // Lot L5: models nobody wrote a derivative for. SLV's risks -- its eight
    // parameters and every point of the leverage -- and Black-Scholes's on
    // three assets (ten parameters, past the duals' eight) equal the CPU
    // tape's.
    TEST_F(GpuRisksTest, SlvRisksMatchTheTape)
    {
        int compared = 0;
        for (const auto &s : library())
        {
            const ScriptedProduct<Real> product(s.source, kCtx);
            if (product.n_underlyings() > 1)
                continue;
            SLVSimModel<Real> slv(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT, skewed<Real>());
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, slv, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << s.name << ": " << why;
            EXPECT_NE(gpu->diagnostics.find("adjoint kernel"), std::string::npos) << gpu->diagnostics;
            const ScriptedProduct<aad::Number> tape_product(s.source, kCtx);
            SLVSimModel<aad::Number> tape_slv(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01),
                                              aad::Number(0.04), aad::Number(1.5), aad::Number(0.05),
                                              aad::Number(0.6), aad::Number(-0.7), kK, kT, skewed<aad::Number>());
            const AADSimulResults cpu =
                simulate_aad(tape_product, tape_slv, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            EXPECT_NEAR(gpu->price, cpu.price, 1e-9 * std::max(1.0, std::abs(cpu.price))) << s.name;
            expect_same_risks(*gpu, cpu, 1e-8, s.name);
            ++compared;
        }
        EXPECT_GT(compared, 20);
    }

    TEST_F(GpuRisksTest, ThreeAssetRisksMatchTheTape)
    {
        int compared = 0;
        for (const auto &s : library())
        {
            const ScriptedProduct<Real> product(s.source, kCtx);
            if (product.n_underlyings() != 3)
                continue;
            auto m = multi_bs<Real>(3);
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, *m, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << s.name << ": " << why;
            EXPECT_NE(gpu->diagnostics.find("10 directions"), std::string::npos) << gpu->diagnostics;
            const ScriptedProduct<aad::Number> tape_product(s.source, kCtx);
            auto tape_model = multi_bs<aad::Number>(3);
            const AADSimulResults cpu =
                simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            EXPECT_NEAR(gpu->price, cpu.price, 1e-9 * std::max(1.0, std::abs(cpu.price))) << s.name;
            expect_same_risks(*gpu, cpu, 1e-8, s.name);
            ++compared;
        }
        EXPECT_GT(compared, 0);
    }

    // With compilation off, what only the generated kernels cover says why,
    // and nothing throws.
    TEST_F(GpuRisksTest, WithoutCompilationSlvRisksStayOnTheCpu)
    {
        const JitSwitch off(false);
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        SLVSimModel<Real> slv(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT, skewed<Real>());
        std::string why;
        EXPECT_FALSE(simulate_script_aad_gpu(product, slv, 1024, 1, why).has_value());
        EXPECT_NE(why.find("QM_SCRIPT_JIT=0"), std::string::npos) << why;
    }

    // WP 27 §14.9: the risks of the last-step expectations from the
    // generated tangent (Black-Scholes, Heston) and adjoint (local vol, SLV)
    // kernels, against the CPU tape on the same draws.
    TEST_F(GpuRisksTest, LastStepRisksMatchTheTape)
    {
        // literal strikes, then strikes the script computes from a fixing
        // (issue #363: Black's formula in the date's block, so the
        // transformations differentiate through the strike too)
        for (const std::string &source :
             {std::string("2026-12-01\n    a = spot()\n2027-06-01\n"
                          "    pays lastStepCall(100) + lastStepPut(95) + lastStepDigital(105) * a / 100\n"),
              std::string("2026-12-01\n    a = spot()\n2027-06-01\n"
                          "    pays lastStepCall(a) + lastStepPut(0.95 * a) + 100 * lastStepDigital(1.05 * a)\n")})
        {
            const ScriptedProduct<Real> product(source, kCtx);
            const ScriptedProduct<aad::Number> tape_product(source, kCtx);
            const auto compare = [&](ISimulationModel<Real> &m, ISimulationModel<aad::Number> &tape_m, const char *what)
            {
                std::string why;
                const auto gpu = simulate_script_aad_gpu(product, m, kPaths, kSeed, why);
                ASSERT_TRUE(gpu.has_value()) << what << ": " << why;
                const AADSimulResults cpu =
                    simulate_aad(tape_product, tape_m, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
                EXPECT_NEAR(gpu->price, cpu.price, 1e-9 * std::max(1.0, std::abs(cpu.price))) << what;
                expect_same_risks(*gpu, cpu, 1e-8, what);
            };
            {
                BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
                BlackScholesSimModel<aad::Number> t(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01),
                                                    aad::Number(0.25));
                compare(m, t, "Black-Scholes");
            }
            {
                auto m = local_vol<Real>();
                auto t = local_vol<aad::Number>();
                compare(*m, *t, "local vol");
            }
            {
                BatesSimModel<Real> m(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0, 0.0, 0.0);
                BatesSimModel<aad::Number> t(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01), aad::Number(0.04),
                                             aad::Number(1.5), aad::Number(0.05), aad::Number(0.6), aad::Number(-0.7),
                                             aad::Number(0.0), aad::Number(0.0), aad::Number(0.0));
                compare(m, t, "Heston");
            }
            {
                SLVSimModel<Real> m(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT, skewed<Real>());
                SLVSimModel<aad::Number> t(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01), aad::Number(0.04),
                                           aad::Number(1.5), aad::Number(0.05), aad::Number(0.6), aad::Number(-0.7), kK,
                                           kT, skewed<aad::Number>());
                compare(m, t, "SLV");
            }
        }
    }

    // WP 26 P2: the risks of a bridge survival from the generated tangent
    // (Black-Scholes) and adjoint (local vol) kernels, against the tape.
    TEST_F(GpuRisksTest, BridgeSurvivalRisksMatchTheTape)
    {
        const std::string source = "2026-12-01\n    a = survivalBelow(135)\n2027-06-01\n"
                                   "    pays a * survivalAbove(80) * max(spot() - 100, 0)\n";
        const ScriptedProduct<Real> product(source, kCtx);
        const ScriptedProduct<aad::Number> tape_product(source, kCtx);
        const auto compare = [&](ISimulationModel<Real> &m, ISimulationModel<aad::Number> &tape_m, const char *what)
        {
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, m, kPaths, kSeed, why);
            ASSERT_TRUE(gpu.has_value()) << what << ": " << why;
            const AADSimulResults cpu =
                simulate_aad(tape_product, tape_m, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
            EXPECT_GT(cpu.price, 0.0) << what;
            EXPECT_NEAR(gpu->price, cpu.price, 1e-9 * std::max(1.0, std::abs(cpu.price))) << what;
            expect_same_risks(*gpu, cpu, 1e-8, what);
        };
        {
            BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
            BlackScholesSimModel<aad::Number> t(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01),
                                                aad::Number(0.25));
            compare(m, t, "Black-Scholes");
        }
        {
            auto m = local_vol<Real>();
            auto t = local_vol<aad::Number>();
            compare(*m, *t, "local vol");
        }
    }

    // With compilation off, a last-step expectation stays on the CPU and
    // says why; nothing reaches the interpreted kernels.
    TEST_F(GpuRisksTest, WithoutCompilationLastStepStaysOnTheCpu)
    {
        const JitSwitch off(false);
        const ScriptedProduct<Real> product("2027-06-01\n    pays lastStepCall(100)\n", kCtx);
        BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
        std::string why;
        EXPECT_FALSE(simulate_script_aad_gpu(product, m, 1024, 1, why).has_value());
        EXPECT_NE(why.find("QM_SCRIPT_JIT=0"), std::string::npos) << why;
    }

    // WP 27 §14.10: second-order risks by the bump on the adjoint, against
    // Black-Scholes's closed forms (S = K = 100, r = 0.03, q = 0.01,
    // vol = 0.25, T = 1). A hard call (its pathwise gamma is exactly 0, so
    // the bump is what gives it one), the same call written with
    // lastStepCall, and local vol on a flat surface, which is Black-Scholes.
    TEST_F(GpuRisksTest, SecondOrderByTheBumpOnTheAdjointIsBlackScholess)
    {
        const double S = 100.0, K = 100.0, r = 0.03, q = 0.01, vol = 0.25, T = 1.0;
        const double d1 = (std::log(S / K) + (r - q + 0.5 * vol * vol) * T) / (vol * std::sqrt(T)),
                     d2 = d1 - vol * std::sqrt(T);
        const double n1 = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * M_PI);
        const double gamma = std::exp(-q * T) * n1 / (S * vol * std::sqrt(T));
        const double vanna = -std::exp(-q * T) * n1 * d2 / vol;
        const double volga = S * std::exp(-q * T) * n1 * std::sqrt(T) * d1 * d2 / vol;

        std::vector<Real> flat;
        for (std::size_t i = 0; i < kK.size() * kT.size(); ++i)
            flat.push_back(vol);
        const auto check = [&](const std::string &source, ISimulationModel<Real> &m, const std::string &what)
        {
            const ScriptedProduct<Real> product(source, kCtx);
            std::string why;
            // 2^18 points: with a hard payoff the vega differences have heavy
            // tails, and 16 replicates of 2^12 points underestimate their
            // error (measured, §14.10); the volga converges all the same.
            const auto so = simulate_script_second_order_gpu(product, m, 1 << 18, kSeed, why);
            ASSERT_TRUE(so.has_value()) << what << ": " << why;
            // the bump's bias is O(h^2): 1 % of the closed form on top of the error
            EXPECT_NEAR(so->gamma, gamma, 4.0 * so->gamma_std_error + 0.01 * gamma) << what << "\n"
                                                                                    << so->method;
            ASSERT_TRUE(so->vanna.has_value()) << what;
            EXPECT_NEAR(*so->vanna, vanna, 4.0 * *so->vanna_std_error + 0.02 * std::abs(vanna)) << what;
            EXPECT_NEAR(*so->volga, volga, 4.0 * *so->volga_std_error + 0.02 * std::abs(volga)) << what;
            EXPECT_LT(so->gamma_std_error, 0.05 * gamma) << what;
            EXPECT_NE(so->method.find("bump on the adjoint"), std::string::npos) << so->method;
        };
        const std::string hard = "2027-06-01\n    pays max(spot() - 100, 0)\n";
        const std::string smooth = "2026-12-01\n    a = spot()\n2027-06-01\n    pays lastStepCall(100)\n";
        {
            BlackScholesSimModel<Real> m(S, r, q, vol);
            check(hard, m, "Black-Scholes, hard call");
        }
        {
            BlackScholesSimModel<Real> m(S, r, q, vol);
            check(smooth, m, "Black-Scholes, lastStepCall");
        }
        {
            LocalVolSimModel<Real> m(S, r, q, kK, kT, flat, 1.0 / 24.0);
            check(hard, m, "flat local vol, hard call");
        }
    }

    // WP 27 §14.16: on a smile curved in the strike, the bump on the GPU's
    // adjoint against the exact route through the C¹ grid (the host's tangent
    // over adjoint, path by path) -- two computations with nothing in common
    // but the model: gamma, and the vanna against a parallel shift (the sum
    // of the spot's row over the grid's nodes).
    TEST_F(GpuRisksTest, SecondOrderBumpUnderACurvedLocalVolIsTheExactRoute)
    {
        namespace ir = scripting::ir;
        const std::string source = "2026-12-01\n    a = spot()\n2027-06-01\n    pays lastStepCall(100)\n";
        std::vector<Real> curved;
        for (Real k : kK)
            for (Real t : kT)
            {
                const double m = (k - 100.0) / 100.0;
                curved.push_back(0.22 - 0.3 * m + 0.6 * m * m + 0.005 * t);
            }
        LocalVolSimModel<Real> model(100.0, 0.03, 0.01, kK, kT, curved, 1.0 / 24.0);
        const ScriptedProduct<Real> product(source, kCtx);
        std::string why;
        const auto so = simulate_script_second_order_gpu(product, model, 1 << 18, kSeed, why);
        ASSERT_TRUE(so.has_value()) << why;
        ASSERT_TRUE(so->vanna.has_value());

        const ir_test::SecondOrderCase sc(source, ir_test::Dyn::LocalVol);
        const auto K = static_cast<std::size_t>(sc.K);
        const auto grid0 = static_cast<std::size_t>(sc.primal.layout.th_grid);
        const auto spot = static_cast<std::size_t>(sc.primal.layout.th_spot);
        std::vector<double> theta = ir::path_data(sc.primal.layout, sc.c.dm, sc.c.product.ir(), sc.c.mats, sc.c.base).theta;
        ASSERT_EQ(theta.size(), grid0 + curved.size());
        std::copy(curved.begin(), curved.end(), theta.begin() + static_cast<std::ptrdiff_t>(grid0));
        const int n = 100000;
        double g1 = 0.0, g2 = 0.0, v1 = 0.0, v2 = 0.0;
        for (int p = 0; p < n; ++p)
        {
            const ir_test::SecondOrder x = sc.at(theta, p);
            const double g = x.hessian[spot * K + spot];
            double v = 0.0;
            for (std::size_t node = grid0; node < theta.size(); ++node)
                v += x.hessian[node * K + spot];
            g1 += g, g2 += g * g, v1 += v, v2 += v * v;
        }
        const auto mean_error = [&](double sum, double squares, double &mean, double &error)
        {
            mean = sum / n;
            error = std::sqrt(std::max(squares / n - mean * mean, 0.0) / n);
        };
        double gamma, gamma_error, vanna, vanna_error;
        mean_error(g1, g2, gamma, gamma_error);
        mean_error(v1, v2, vanna, vanna_error);
        std::printf("curved local vol, lastStepCall: gamma exact %.6f +- %.6f, bump %.6f +- %.6f; vanna exact %.5f "
                    "+- %.5f, bump %.5f +- %.5f\n",
                    gamma, gamma_error, so->gamma, so->gamma_std_error, vanna, vanna_error, *so->vanna,
                    *so->vanna_std_error);
        EXPECT_GT(gamma, 0.0);
        // the bump's bias is O(h^2): 1 % on top of the two errors
        EXPECT_NEAR(so->gamma, gamma, 4.0 * (so->gamma_std_error + gamma_error) + 0.01 * gamma);
        EXPECT_NEAR(*so->vanna, vanna, 4.0 * (*so->vanna_std_error + vanna_error) + 0.01 * std::abs(vanna));
    }

    // Heston has no single vol: gamma and the spot's row only.
    TEST_F(GpuRisksTest, SecondOrderUnderHestonGivesGammaOnly)
    {
        const ScriptedProduct<Real> product("2027-06-01\n    pays max(spot() - 100, 0)\n", kCtx);
        BatesSimModel<Real> m(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0, 0.0, 0.0);
        std::string why;
        const auto so = simulate_script_second_order_gpu(product, m, 1 << 16, kSeed, why);
        ASSERT_TRUE(so.has_value()) << why;
        EXPECT_GT(so->gamma, 0.0);
        EXPECT_LT(so->gamma_std_error, 0.05 * so->gamma);
        EXPECT_FALSE(so->vanna.has_value());
        EXPECT_EQ(so->spot_row.size(), m.parameter_labels().size());
    }

    // WP 27 §14.11: the Vibrato kernel is the host's ir::run_path_vibrato --
    // on the same Philox paths its risks are the host's mean to rounding,
    // under Black-Scholes and local vol, on a hard digital. And the
    // dispatcher runs Vibrato on the GPU for a payoff that jumps.
    TEST_F(GpuRisksTest, VibratoKernelIsTheHostsVibrato)
    {
        namespace ir = scripting::ir;
        const std::string source = "2026-12-01\n    a = spot()\n2027-06-01\n    if spot() > 100 then pays 1 endIf\n";
        const ScriptedProduct<Real> product(source, kCtx);
        const std::size_t paths = 8192;
        const auto compare = [&](ISimulationModel<Real> &m, const char *what)
        {
            std::string why;
            const auto gpu = simulate_script_aad_gpu(product, m, paths, kSeed, why, SamplerKind::PseudoRandom, 0, true);
            ASSERT_TRUE(gpu.has_value()) << what << ": " << why;
            EXPECT_NE(gpu->diagnostics.find("Vibrato kernel"), std::string::npos) << gpu->diagnostics;
            m.init(product.timeline(), product.defline());
            DeviceModel dm;
            ASSERT_TRUE(m.describe_device(dm));
            const ir::PathProgram primal = ir::path_program(product.ir(), dm.kind, 1, true, true);
            const int K = primal.layout.n_risks;
            std::vector<std::int32_t> dirs(static_cast<std::size_t>(K));
            for (int k = 0; k < K; ++k)
                dirs[static_cast<std::size_t>(k)] = k;
            const ir::PathProgram tan = ir::tangent(primal, dirs);
            std::vector<std::vector<Time>> mats;
            for (const SampleDef &def : product.defline())
                mats.push_back(def.discount_mats);
            const ir::PathData data = ir::path_data(primal.layout, dm, product.ir(), mats, product.baseline());
            std::vector<WelfordAccumulator> host(static_cast<std::size_t>(K));
            std::vector<double> d(static_cast<std::size_t>(K));
            ir::DerivativeScratch w;
            mc::PathDraws draws;
            draws.seed = kSeed;
            draws.stride = dm.stride;
            WelfordAccumulator price;
            for (std::size_t p = 0; p < paths; ++p)
            {
                draws.begin(p);
                price.add(ir::run_path_vibrato(tan, K, data, dm, product.ir(), draws, w, d.data()));
                for (int k = 0; k < K; ++k)
                    host[static_cast<std::size_t>(k)].add(d[static_cast<std::size_t>(k)]);
            }
            EXPECT_NEAR(gpu->price, price.mean, 1e-10) << what;
            for (int k = 0; k < K; ++k)
                EXPECT_NEAR(gpu->risks[static_cast<std::size_t>(k)], host[static_cast<std::size_t>(k)].mean,
                            1e-8 * std::max(1.0, std::abs(host[static_cast<std::size_t>(k)].mean)))
                    << what << " " << gpu->risk_labels[static_cast<std::size_t>(k)];
        };
        {
            BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
            compare(m, "Black-Scholes");
        }
        {
            auto m = local_vol<Real>();
            compare(*m, "local vol");
        }
        // the dispatcher, a realistic local vol (252 steps a year), timed
        LocalVolSimModel<Real> daily(100.0, 0.03, 0.01, kK, kT, skewed<Real>(), 1.0 / 252.0);
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        const auto so = simulate_script_second_order(product, daily, 200000, kSeed, why);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(so.has_value()) << why;
        EXPECT_NE(so->method.find("Vibrato on the GPU"), std::string::npos) << so->method;
        // an at-the-money one-year digital's gamma is near 0 here: an absolute bound
        EXPECT_LT(so->gamma_std_error, 2e-4);
        std::printf("Vibrato second order, local vol 252 steps/yr, 200000 points: %.0f ms, gamma %.6f +- %.6f\n", ms,
                    so->gamma, so->gamma_std_error);
    }

    // WP 26 §16 (#282 step 3): exercise() / call() on the GPU, in the kernels
    // compiled from the IR. The rule is fitted once on the CPU; under it the
    // GPU's price is the CPU's on the same Philox paths, and its risks
    // (tangent under Black-Scholes and Heston, adjoint under local vol) the
    // tape's, the decision held fixed on both.
    namespace
    {
        const std::vector<std::string> &exercise_scripts()
        {
            static const std::vector<std::string> s = {
                "2026-07-01 2026-08-03 2026-09-01 2026-10-01 2026-11-02 2026-12-01 2027-01-04 2027-02-01\n"
                "    if exercise(max(100 - spot(), 0), spot()) then\n        pays max(100 - spot(), 0)\n"
                "    endIf\n",
                "2026-12-01\n    if exercise(spot()) then\n        is_call = 1\n    endIf\n"
                "2027-06-01\n    if is_call = 1 then\n        pays max(spot() - 100, 0)\n    else\n"
                "        pays max(100 - spot(), 0)\n    endIf\n",
                "2026-12-01 2027-06-01 2027-12-01\n    if done = 0 then\n        pays 2\n"
                "        if call(spot()) then\n            pays 100\n            done = 1\n        endIf\n"
                "    endIf\n2028-06-01\n    if done = 0 then\n        pays 104 * min(1, spot() / 90)\n    endIf\n",
                "2026-09-01 2026-12-01 2027-03-01 2027-06-01\n    if spot() < 98 then\n"
                "        if exercise(spot()) then\n            pays 100 - spot()\n        endIf\n    endIf\n"};
            return s;
        }

        template <class T>
        std::unique_ptr<ISimulationModel<T>> exercise_model(int which)
        {
            switch (which)
            {
                case 1:
                    return local_vol<T>();
                case 2:
                    return std::make_unique<BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05),
                                                              T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0));
                default:
                    return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25));
            }
        }
        const char *const kExerciseModels[] = {"Black-Scholes", "local vol", "Heston"};
    } // namespace

    TEST_F(GpuRisksTest, EarlyExercisePricesAreTheCpus)
    {
        for (const std::string &source : exercise_scripts())
            for (const bool fuzzy : {false, true})
                for (int which = 0; which < 3; ++which)
                {
                    ScriptSettings ss;
                    ss.fuzzy = fuzzy;
                    ScriptedProduct<Real> product(source, kCtx, ss);
                    auto fit_model = exercise_model<Real>(which);
                    fit_exercise_policy(product, *fit_model, 16384, 5);
                    ASSERT_FALSE(product.ir().empty()) << product.ir_refusal();
                    const std::string what = std::string(kExerciseModels[which]) + (fuzzy ? ", fuzzy\n" : "\n") + source;
                    PricingSettings set;
                    set.mc_paths = 16384;
                    set.mc_seed = 5;
                    set.mc_rng = RngKind::Philox;
                    set.mc_sampler = SamplerKind::PseudoRandom;
                    auto cpu_model = exercise_model<Real>(which);
                    auto gpu_model = exercise_model<Real>(which);
                    set.mc_device = ComputeDevice::Cpu;
                    const SimulationMCResult cpu = simulate_script(product, *cpu_model, set);
                    set.mc_device = ComputeDevice::Gpu;
                    const SimulationMCResult gpu = simulate_script(product, *gpu_model, set);
                    ASSERT_EQ(gpu.device, "gpu") << what << gpu.diagnostics;
                    EXPECT_NE(gpu.diagnostics.find("compiled script"), std::string::npos) << gpu.diagnostics;
                    EXPECT_NE(cpu.npv(), 0.0) << what;
                    EXPECT_NEAR(gpu.npv(), cpu.npv(), 1e-9 * std::max(1.0, std::abs(cpu.npv()))) << what;
                    EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-6 * std::max(1e-12, cpu.std_error())) << what;
                }
    }

    TEST_F(GpuRisksTest, EarlyExerciseRisksMatchTheTape)
    {
        for (const std::string &source : exercise_scripts())
            for (const bool fuzzy : {false, true})
                for (int which = 0; which < 3; ++which)
                {
                    ScriptSettings ss;
                    ss.fuzzy = fuzzy;
                    ScriptedProduct<Real> product(source, kCtx, ss);
                    auto fit_model = exercise_model<Real>(which);
                    fit_exercise_policy(product, *fit_model, 16384, 5);
                    const std::string what = std::string(kExerciseModels[which]) + (fuzzy ? ", fuzzy\n" : "\n") + source;
                    auto model = exercise_model<Real>(which);
                    std::string why;
                    const auto gpu = simulate_script_aad_gpu(product, *model, kPaths, kSeed, why);
                    ASSERT_TRUE(gpu.has_value()) << what << why;
                    ScriptedProduct<aad::Number> tape_product(source, kCtx, ss);
                    tape_product.set_exercise_policy(product.exercise_policy());
                    auto tape_model = exercise_model<aad::Number>(which);
                    const AADSimulResults cpu =
                        simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff, RngKind::Philox);
                    EXPECT_NEAR(gpu->price, cpu.price, 1e-9 * std::max(1.0, std::abs(cpu.price))) << what;
                    expect_same_risks(*gpu, cpu, 1e-8, what);
                }
    }

    // Without compilation the interpreted kernels have no rule: the GPU
    // refuses, the CPU prices and says why.
    TEST_F(GpuRisksTest, WithoutCompilationEarlyExerciseStaysOnTheCpu)
    {
        const JitSwitch off(false);
        ScriptedProduct<Real> product(exercise_scripts().front(), kCtx);
        BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
        fit_exercise_policy(product, m, 4096, 5);
        PricingSettings set;
        set.mc_paths = 4096;
        set.mc_device = ComputeDevice::Gpu;
        EXPECT_THROW(simulate_script(product, m, set), InvalidInput);
        set.mc_device = ComputeDevice::Auto;
        const SimulationMCResult r = simulate_script(product, m, set);
        EXPECT_EQ(r.device, "cpu");
        EXPECT_NE(r.diagnostics.find("QM_SCRIPT_JIT=0"), std::string::npos) << r.diagnostics;
    }

} // namespace quantModeling
