#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>

#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/thread_pool.hpp"

namespace quantModeling
{

    // ---------------------------------------------------------------------
    // Logical-block reduction (blueprint/wp/19-gpu.md §5, §7, ADR-G5)
    // ---------------------------------------------------------------------

    namespace
    {
        struct ScalarStats
        {
            WelfordAccumulator acc;
            void add(double x) { acc.add(x); }
            void merge(const ScalarStats &o) { acc.merge(o.acc); }
        };

        struct LogNormalUnit
        {
            uint64_t seed;
            double operator()(uint64_t u) const
            {
                return std::exp(0.3 * inverse_normal_cdf(philox_uniform(seed, u, 0)));
            }
        };
    } // namespace

    // The tree of Chan merges gives the same statistics as one sequential
    // Welford pass, up to rounding -- including a ragged last block.
    TEST(LogicalBlocks, TreeMatchesSequentialWelford)
    {
        const uint64_t n = 3 * mc::LogicalBlocks::kUnitsPerBlock + 1234;
        const LogNormalUnit fn{7};
        WelfordAccumulator seq;
        for (uint64_t u = 0; u < n; ++u)
            seq.add(fn(u));

        const ScalarStats tree = mc::reduce_logical_blocks<ScalarStats>(n, fn);
        EXPECT_EQ(tree.acc.n, static_cast<long long>(n));
        EXPECT_NEAR(tree.acc.mean, seq.mean, 1e-14 * std::abs(seq.mean));
        EXPECT_NEAR(tree.acc.variance(), seq.variance(), 1e-12 * seq.variance());
    }

    // The order of additions is fixed by the unit indices: spreading blocks
    // over a thread pool changes nothing, bit for bit.
    TEST(LogicalBlocks, ThreadPoolIsBitIdentical)
    {
        const uint64_t n = 37 * mc::LogicalBlocks::kUnitsPerBlock + 99;
        const LogNormalUnit fn{11};
        const ScalarStats serial = mc::reduce_logical_blocks<ScalarStats>(n, fn);

        ThreadPool pool;
        pool.start(5);
        const ScalarStats parallel = mc::reduce_logical_blocks<ScalarStats>(n, fn, &pool);
        pool.stop();

        EXPECT_EQ(parallel.acc.n, serial.acc.n);
        EXPECT_EQ(parallel.acc.mean, serial.acc.mean);
        EXPECT_EQ(parallel.acc.m2, serial.acc.m2);
    }

    TEST(LogicalBlocks, EmptyAndTinyInputs)
    {
        const LogNormalUnit fn{3};
        EXPECT_EQ(mc::reduce_logical_blocks<ScalarStats>(0, fn).acc.n, 0);
        const ScalarStats one = mc::reduce_logical_blocks<ScalarStats>(1, fn);
        EXPECT_EQ(one.acc.n, 1);
        EXPECT_EQ(one.acc.mean, fn(0));
    }

    // ---------------------------------------------------------------------
    // Vanilla MC engine with the counter-based generator on the CPU
    // ---------------------------------------------------------------------

    class PhiloxVanillaTest : public ::testing::Test
    {
      protected:
        std::shared_ptr<const BlackScholesModel> model =
            std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);

        PricingResult price(OptionType type, Real K, PricingSettings s) const
        {
            PricingContext ctx;
            ctx.model = model;
            ctx.settings = s;
            VanillaOption option(std::make_shared<PlainVanillaPayoff>(type, K),
                                 std::make_shared<EuropeanExercise>(1.0));
            BSEuroVanillaMCEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        PricingResult exact(OptionType type, Real K) const
        {
            PricingContext ctx;
            ctx.model = model;
            VanillaOption option(std::make_shared<PlainVanillaPayoff>(type, K),
                                 std::make_shared<EuropeanExercise>(1.0));
            BSEuroVanillaAnalyticEngine engine(ctx);
            option.accept(engine);
            return engine.results();
        }

        static PricingSettings philox(bool antithetic, bool is = false)
        {
            PricingSettings s;
            s.mc_paths = 1 << 18;
            s.mc_seed = 2024;
            s.mc_antithetic = antithetic;
            s.mc_importance_sampling = is;
            s.mc_rng = RngKind::Philox;
            return s;
        }
    };

    TEST_F(PhiloxVanillaTest, UnbiasedAgainstClosedForm)
    {
        for (OptionType type : {OptionType::Call, OptionType::Put})
            for (bool anti : {false, true})
            {
                const PricingResult mc = price(type, 105.0, philox(anti));
                const PricingResult bs = exact(type, 105.0);
                EXPECT_NEAR(mc.npv, bs.npv, 4.0 * mc.mc_std_error);
                EXPECT_NEAR(*mc.greeks.delta, *bs.greeks.delta, 4.0 * *mc.greeks.delta_std_error);
                EXPECT_NEAR(*mc.greeks.vega, *bs.greeks.vega, 4.0 * *mc.greeks.vega_std_error);
                EXPECT_NE(mc.diagnostics.find("Philox"), std::string::npos);
            }
    }

    TEST_F(PhiloxVanillaTest, ImportanceSamplingUnbiasedFarOutOfTheMoney)
    {
        const PricingResult mc = price(OptionType::Call, 160.0, philox(false, true));
        const PricingResult bs = exact(OptionType::Call, 160.0);
        EXPECT_NEAR(mc.npv, bs.npv, 4.0 * mc.mc_std_error);
    }

    // With no device, Auto falls back to the CPU and its configured
    // generator; Gpu refuses rather than silently running elsewhere.
    TEST_F(PhiloxVanillaTest, DeviceRoutingWithoutGpu)
    {
        if (gpu::device_count() > 0)
            GTEST_SKIP() << "a GPU is present; covered by tests/gpu";

        PricingSettings s = philox(true);
        s.mc_rng = RngKind::Pcg32;
        const PricingResult cpu = price(OptionType::Call, 100.0, s);
        s.mc_device = ComputeDevice::Auto;
        EXPECT_EQ(price(OptionType::Call, 100.0, s).npv, cpu.npv);
        s.mc_device = ComputeDevice::Gpu;
        EXPECT_THROW(price(OptionType::Call, 100.0, s), gpu::GpuUnavailable);
    }

} // namespace quantModeling
