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

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
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
                EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-6 * std::max(1e-12, cpu.std_error()))
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

    // Where the GPU has no risks: SLV says why, and nothing throws.
    TEST_F(GpuRisksTest, SlvRisksStayOnTheCpu)
    {
        const ScriptedProduct<Real> product(library().front().source, kCtx);
        SLVSimModel<Real> slv(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT, skewed<Real>());
        std::string why;
        EXPECT_FALSE(simulate_script_aad_gpu(product, slv, 1024, 1, why).has_value());
        EXPECT_NE(why.find("SLV"), std::string::npos) << why;
    }

} // namespace quantModeling
