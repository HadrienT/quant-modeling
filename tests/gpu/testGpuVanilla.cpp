#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/rng.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/sobol.hpp"

// Lot G0 of blueprint/wp/19-gpu.md: the vanilla kernel on the V100s.
// Built only with QM_ENABLE_CUDA, labelled `gpu` (ctest -L gpu).

namespace quantModeling
{

    namespace
    {
        mc::VanillaTerminalSpec make_spec(Real S0, Real K, Real r, Real q, Real sigma, Real T)
        {
            mc::VanillaTerminalSpec s;
            s.S0 = S0;
            s.K = K;
            s.sigma = sigma;
            s.T = T;
            s.sqrtT = std::sqrt(T);
            s.movedSpot = S0 * std::exp((r - q - 0.5 * sigma * sigma) * T);
            s.rootVariance = sigma * s.sqrtT;
            s.df = std::exp(-r * T);
            s.dS = 0.01 * S0;
            s.factor_up = 1.01;
            s.factor_dn = 0.99;
            s.theta_bump = 1.0 / 365.0;
            s.movedSpot_upT = S0 * std::exp((r - q - 0.5 * sigma * sigma) * (T + s.theta_bump));
            s.movedSpot_dnT = S0 * std::exp((r - q - 0.5 * sigma * sigma) * (T - s.theta_bump));
            s.rootVariance_upT = sigma * std::sqrt(T + s.theta_bump);
            s.rootVariance_dnT = sigma * std::sqrt(T - s.theta_bump);
            s.df_upT = std::exp(-r * (T + s.theta_bump));
            s.df_dnT = std::exp(-r * (T - s.theta_bump));
            return s;
        }

        template <OptionType CP, bool A, bool IS>
        mc::VanillaStats cpu_run(const gpu::VanillaGpuRequest &req)
        {
            const mc::VanillaPhiloxUnit<CP, A, IS> unit{req.spec, req.seed, req.is_shift};
            return mc::reduce_logical_blocks<mc::VanillaStats>(req.n_units, unit);
        }

        mc::VanillaStats cpu_twin(const gpu::VanillaGpuRequest &req)
        {
            const bool is = req.is_shift != 0.0;
            if (req.type == OptionType::Call)
            {
                if (req.antithetic)
                    return is ? cpu_run<OptionType::Call, true, true>(req) : cpu_run<OptionType::Call, true, false>(req);
                return is ? cpu_run<OptionType::Call, false, true>(req) : cpu_run<OptionType::Call, false, false>(req);
            }
            if (req.antithetic)
                return is ? cpu_run<OptionType::Put, true, true>(req) : cpu_run<OptionType::Put, true, false>(req);
            return is ? cpu_run<OptionType::Put, false, true>(req) : cpu_run<OptionType::Put, false, false>(req);
        }

        void expect_same_bits(const WelfordAccumulator &a, const WelfordAccumulator &b)
        {
            EXPECT_EQ(a.n, b.n);
            EXPECT_EQ(a.mean, b.mean);
            EXPECT_EQ(a.m2, b.m2);
        }

        void expect_same_bits(const mc::VanillaStats &a, const mc::VanillaStats &b)
        {
            expect_same_bits(a.payoff, b.payoff);
            expect_same_bits(a.delta, b.delta);
            expect_same_bits(a.vega, b.vega);
            expect_same_bits(a.rho, b.rho);
            expect_same_bits(a.gamma, b.gamma);
            expect_same_bits(a.theta, b.theta);
        }

        /// |a - b| relative to the Monte-Carlo standard error: "the same
        /// number" up to the last-ulp differences of exp/log/erfc between
        /// CUDA's libdevice and glibc (blueprint/wp/19-gpu.md §7).
        void expect_cpu_equivalent(const WelfordAccumulator &gpu, const WelfordAccumulator &cpu)
        {
            EXPECT_EQ(gpu.n, cpu.n);
            EXPECT_NEAR(gpu.mean, cpu.mean, 1e-12 * std::max(1.0, std::abs(cpu.mean)));
            EXPECT_NEAR(gpu.std_error(), cpu.std_error(), 1e-10 * cpu.std_error());
        }
    } // namespace

    class GpuVanillaTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (gpu::device_count() == 0)
                GTEST_SKIP() << "no CUDA device";
        }
    };

    TEST_F(GpuVanillaTest, DeviceIsDetected)
    {
        EXPECT_TRUE(gpu::compiled_with_cuda());
        EXPECT_FALSE(gpu::device_name(0).empty());
        EXPECT_GT(gpu::free_memory(0), std::size_t{0});
    }

    // The draws themselves are integer arithmetic: bit-identical to the CPU,
    // including path indices above 2^32.
    TEST_F(GpuVanillaTest, PhiloxDrawsAreBitIdenticalToCpu)
    {
        constexpr uint64_t seed = 0xfeedbeef1234ull;
        for (uint64_t first : {uint64_t{0}, (uint64_t{1} << 32) - 3})
        {
            const uint32_t n_paths = 4099, draws = 5;
            const std::vector<double> d = gpu::philox_uniforms(seed, first, n_paths, draws);
            ASSERT_EQ(d.size(), std::size_t{n_paths} * draws);
            for (uint32_t p = 0; p < n_paths; ++p)
                for (uint32_t j = 0; j < draws; ++j)
                    ASSERT_EQ(d[std::size_t{p} * draws + j], philox_uniform(seed, first + p, j))
                        << "path " << first + p << " draw " << j;
        }
    }

    // GPU = CPU with the same generator, for every kernel instantiation. The
    // draws agree bit for bit; the results agree to a few ulps, because
    // CUDA's exp/erfc are not glibc's and nvcc contracts a*b+c into FMAs.
    TEST_F(GpuVanillaTest, MatchesCpuTwin)
    {
        gpu::VanillaGpuRequest req;
        req.spec = make_spec(100.0, 110.0, 0.05, 0.02, 0.25, 1.5);
        req.n_units = 5 * mc::LogicalBlocks::kUnitsPerBlock + 777;
        req.seed = 99;
        for (OptionType type : {OptionType::Call, OptionType::Put})
            for (bool anti : {false, true})
                for (Real is : {0.0, 0.4})
                {
                    req.type = type;
                    req.antithetic = anti;
                    req.is_shift = is;
                    const mc::VanillaStats g = gpu::simulate_vanilla_terminal(req);
                    const mc::VanillaStats c = cpu_twin(req);
                    expect_cpu_equivalent(g.payoff, c.payoff);
                    expect_cpu_equivalent(g.delta, c.delta);
                    expect_cpu_equivalent(g.vega, c.vega);
                    expect_cpu_equivalent(g.gamma, c.gamma);
                    expect_cpu_equivalent(g.theta, c.theta);
                }
    }

    // The order of additions is fixed by the indices: the number of kernel
    // launches, the device, and the run do not change a bit.
    TEST_F(GpuVanillaTest, BitIdenticalAcrossLaunchesAndDevices)
    {
        gpu::VanillaGpuRequest req;
        req.spec = make_spec(100.0, 95.0, 0.03, 0.0, 0.3, 0.75);
        req.n_units = 40 * mc::LogicalBlocks::kUnitsPerBlock + 5;
        req.seed = 7;
        const mc::VanillaStats ref = gpu::simulate_vanilla_terminal(req);

        expect_same_bits(gpu::simulate_vanilla_terminal(req), ref);

        req.max_blocks_per_launch = 3;
        expect_same_bits(gpu::simulate_vanilla_terminal(req), ref);

        if (gpu::device_count() > 1)
        {
            req.device = 1;
            req.max_blocks_per_launch = 0;
            expect_same_bits(gpu::simulate_vanilla_terminal(req), ref);
        }
    }

    TEST_F(GpuVanillaTest, EngineOnGpuAgainstClosedFormAndCpu)
    {
        const auto model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
        VanillaOption option(std::make_shared<PlainVanillaPayoff>(OptionType::Call, 100.0),
                             std::make_shared<EuropeanExercise>(1.0));
        PricingContext ctx;
        ctx.model = model;
        ctx.settings.mc_paths = 1 << 22;
        ctx.settings.mc_seed = 5;
        ctx.settings.mc_antithetic = true;

        ctx.settings.mc_device = ComputeDevice::Gpu;
        BSEuroVanillaMCEngine on_gpu(ctx);
        option.accept(on_gpu);
        const PricingResult g = on_gpu.results();
        EXPECT_NE(g.diagnostics.find("on GPU"), std::string::npos) << g.diagnostics;

        ctx.settings.mc_device = ComputeDevice::Auto;
        BSEuroVanillaMCEngine on_auto(ctx);
        option.accept(on_auto);
        EXPECT_EQ(on_auto.results().npv, g.npv);

        ctx.settings.mc_device = ComputeDevice::Cpu;
        ctx.settings.mc_rng = RngKind::Philox;
        BSEuroVanillaMCEngine on_cpu(ctx);
        option.accept(on_cpu);
        EXPECT_NEAR(g.npv, on_cpu.results().npv, 1e-12 * g.npv);

        BSEuroVanillaAnalyticEngine bs(ctx);
        option.accept(bs);
        EXPECT_NEAR(g.npv, bs.results().npv, 4.0 * g.mc_std_error);
        EXPECT_NEAR(*g.greeks.delta, *bs.results().greeks.delta, 4.0 * *g.greeks.delta_std_error);
    }

    // Sobol on the device (lot G1): every one of the 21 201 dimensions gives
    // the CPU's bits, at the start of the sequence and far into it.
    TEST_F(GpuVanillaTest, SobolPointsAreBitIdenticalToCpu)
    {
        constexpr int dim = sobol_detail::kMaxDimension;
        for (uint32_t first : {0u, (1u << 30) + 7u})
        {
            constexpr uint32_t n_points = 12;
            const std::vector<double> d = gpu::sobol_uniforms(dim, 99, first, n_points);
            SobolSequence seq(dim, 99);
            seq.skip_to(first);
            std::vector<double> point(dim);
            for (uint32_t p = 0; p < n_points; ++p)
            {
                seq.next_uniform(point);
                for (int k = 0; k < dim; ++k)
                    ASSERT_EQ(d[std::size_t{p} * dim + static_cast<std::size_t>(k)], point[static_cast<std::size_t>(k)])
                        << "point " << first + p << " dim " << k;
            }
        }
    }

    // The vanilla engine's Sobol RQMC on the GPU: each replicate draws the
    // CPU replicate's points, so the prices agree to a few ulps.
    TEST_F(GpuVanillaTest, SobolEngineOnGpuMatchesCpu)
    {
        PricingContext ctx;
        ctx.model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
        ctx.settings.mc_paths = 1 << 20;
        ctx.settings.mc_sampler = SamplerKind::Sobol;
        VanillaOption option(std::make_shared<PlainVanillaPayoff>(OptionType::Put, 95.0),
                             std::make_shared<EuropeanExercise>(0.5));
        BSEuroVanillaMCEngine cpu(ctx);
        option.accept(cpu);
        ctx.settings.mc_device = ComputeDevice::Gpu;
        BSEuroVanillaMCEngine on_gpu(ctx);
        option.accept(on_gpu);
        EXPECT_EQ(on_gpu.results().device, "gpu");
        EXPECT_NE(on_gpu.results().diagnostics.find("Sobol RQMC"), std::string::npos);
        EXPECT_NEAR(on_gpu.results().npv, cpu.results().npv, 1e-12 * cpu.results().npv);
        EXPECT_NEAR(on_gpu.results().mc_std_error, cpu.results().mc_std_error, 1e-6 * cpu.results().mc_std_error);
    }

    TEST_F(GpuVanillaTest, StratifiedOnGpuIsRefusedNotRerouted)
    {
        PricingContext ctx;
        ctx.model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
        ctx.settings.mc_paths = 1 << 12;
        ctx.settings.mc_sampler = SamplerKind::Stratified;
        ctx.settings.mc_device = ComputeDevice::Gpu;
        VanillaOption option(std::make_shared<PlainVanillaPayoff>(OptionType::Call, 100.0),
                             std::make_shared<EuropeanExercise>(1.0));
        BSEuroVanillaMCEngine engine(ctx);
        EXPECT_THROW(option.accept(engine), InvalidInput);
    }

} // namespace quantModeling
