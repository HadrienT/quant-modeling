#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/paths.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/utils/philox.hpp"

// Lot G1 of blueprint/wp/19-gpu.md: the models as flat functors. The CPU
// model and the GPU kernel call the same step code; fed the same draws they
// follow the same path, which is stronger than "the same law".

namespace quantModeling
{

    namespace
    {
        constexpr uint64_t kSeed = 0xC0FFEE;
        constexpr uint32_t kPaths = 4096;

        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.1, 0.5, 1.0, 2.0};

        /// A skewed surface (vol or leverage), K-major.
        std::vector<Real> skewed(double level, double skew)
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(level + skew * (100.0 - k) / 100.0 + 0.01 * t);
            return g;
        }

        /// The CPU model's terminal spot on path p, its gaussians taken from
        /// Philox as the GPU takes them: factor f of step s is draw s*F + f;
        /// `stride` > F pads each step (Bates's two jump draws, unused at λ = 0).
        double cpu_terminal(const ISimulationModel<Real> &model, uint32_t p, int factors, int stride)
        {
            const std::size_t n = model.sim_timeline().size();
            std::vector<double> g(n * static_cast<std::size_t>(stride), 0.0);
            for (std::size_t s = 0; s < n; ++s)
                for (int f = 0; f < factors; ++f)
                    g[s * static_cast<std::size_t>(stride) + static_cast<std::size_t>(f)] = inverse_normal_cdf(
                        philox_uniform(kSeed, p, static_cast<uint32_t>(s * static_cast<std::size_t>(factors)) + static_cast<uint32_t>(f)));
            Scenario<Real> path(1);
            model.generate_path(g, path);
            return path.back().spots[0];
        }

        void expect_same_paths(const ISimulationModel<Real> &cpu, const gpu::PathModelSpec &spec, int stride)
        {
            const std::vector<Time> times(cpu.sim_timeline().begin(), cpu.sim_timeline().end());
            const std::vector<double> gpu_S = gpu::terminal_spots(spec, times, kPaths, kSeed);
            ASSERT_EQ(gpu_S.size(), kPaths);
            double mean = 0.0;
            for (uint32_t p = 0; p < kPaths; ++p)
            {
                const double c = cpu_terminal(cpu, p, spec.factors(), stride);
                ASSERT_NEAR(gpu_S[p], c, 1e-11 * c) << "path " << p;
                mean += gpu_S[p];
            }
            // and the law: the discounted spot is a martingale
            mean /= kPaths;
            double var = 0.0;
            for (double s : gpu_S)
                var += (s - mean) * (s - mean);
            const double se = std::sqrt(var / (kPaths - 1) / kPaths);
            const double fwd = spec.s0 * std::exp((spec.r - spec.q) * times.back());
            EXPECT_NEAR(mean, fwd, 4.0 * se);
        }

        const TimeLine kProductDates{0.5, 1.0};
        const std::vector<SampleDef> kDefline(1);
    } // namespace

    class GpuPathsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuPathsTest, LocalVolFollowsTheCpuModel)
    {
        LocalVolSimModel<Real> cpu(100.0, 0.03, 0.01, kK, kT, skewed(0.2, 0.3), 1.0 / 52.0);
        cpu.init(TimeLine{1.0}, kDefline);
        gpu::PathModelSpec spec;
        spec.kind = gpu::PathModel::LocalVol;
        spec.s0 = 100.0;
        spec.r = 0.03;
        spec.q = 0.01;
        spec.K = kK;
        spec.T_grid = kT;
        spec.grid = skewed(0.2, 0.3);
        expect_same_paths(cpu, spec, 1);
    }

    TEST_F(GpuPathsTest, HestonFollowsTheCpuModel)
    {
        // The scripts' Heston is Bates without jumps.
        BatesSimModel<Real> cpu(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0, 0.0, 0.0, 1.0 / 52.0);
        cpu.init(TimeLine{1.0}, kDefline);
        gpu::PathModelSpec spec;
        spec.kind = gpu::PathModel::Heston;
        spec.s0 = 100.0;
        spec.r = 0.03;
        spec.q = 0.01;
        spec.heston = {0.04, 1.5, 0.05, 0.6, -0.7};
        expect_same_paths(cpu, spec, 4);
    }

    TEST_F(GpuPathsTest, SlvFollowsTheCpuModel)
    {
        SLVSimModel<Real> cpu(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT, skewed(1.0, 0.5), 1.0 / 52.0);
        cpu.init(TimeLine{1.0}, kDefline);
        gpu::PathModelSpec spec;
        spec.kind = gpu::PathModel::SLV;
        spec.s0 = 100.0;
        spec.r = 0.03;
        spec.q = 0.01;
        spec.heston = {0.04, 1.5, 0.05, 0.6, -0.7};
        spec.K = kK;
        spec.T_grid = kT;
        spec.grid = skewed(1.0, 0.5);
        expect_same_paths(cpu, spec, 2);
    }

} // namespace quantModeling
