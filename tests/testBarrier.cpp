#include <gtest/gtest.h>

#include "quantModeling/instruments/base.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {

        constexpr Real S0 = 100.0;
        constexpr Real K = 100.0;
        constexpr Real T = 1.0;
        constexpr Real r = 0.05;
        constexpr Real q = 0.02;
        constexpr Real sigma = 0.20;

        // ── Helper ───────────────────────────────────────────────────────────
        PricingResult priceBarrier(bool is_call,
                                   BarrierType bt,
                                   Real barrier_level,
                                   Real rebate = 0.0,
                                   int n_paths = 200000,
                                   int seed = 42,
                                   bool brownian_bridge = true)
        {
            BarrierBSInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.vol = sigma;
            in.is_call = is_call;
            in.barrier_type = bt;
            in.barrier_level = barrier_level;
            in.rebate = rebate;
            in.n_paths = n_paths;
            in.seed = seed;
            in.brownian_bridge = brownian_bridge;

            PricingRequest request{
                InstrumentKind::EquityBarrierOption,
                ModelKind::BlackScholes,
                EngineKind::MonteCarlo,
                PricingInput{in}};
            return default_registry().price(request);
        }

        PricingResult priceVanillaAnalytic(bool is_call)
        {
            VanillaBSInput in{S0, K, T, r, q, sigma, is_call};
            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::Analytic,
                PricingInput{in}};
            return default_registry().price(request);
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  In + Out = Vanilla parity
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, DownInPlusDownOutEqualsVanilla)
    {
        // Down-and-in call + Down-and-out call = vanilla call
        const Real di = priceBarrier(true, BarrierType::DownAndIn, 80.0, 0.0, 300000, 1).npv;
        const Real do_ = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 300000, 1).npv;
        const Real vanilla = priceVanillaAnalytic(true).npv;

        EXPECT_NEAR(di + do_, vanilla, vanilla * 0.05)
            << "DI + DO should approximate the vanilla call";
    }

    TEST(BarrierMC, UpInPlusUpOutEqualsVanilla)
    {
        const Real ui = priceBarrier(true, BarrierType::UpAndIn, 120.0, 0.0, 300000, 1).npv;
        const Real uo = priceBarrier(true, BarrierType::UpAndOut, 120.0, 0.0, 300000, 1).npv;
        const Real vanilla = priceVanillaAnalytic(true).npv;

        EXPECT_NEAR(ui + uo, vanilla, vanilla * 0.05)
            << "UI + UO should approximate the vanilla call";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Basic price properties
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, DownAndOutCallIsPositive)
    {
        const auto res = priceBarrier(true, BarrierType::DownAndOut, 80.0);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(BarrierMC, DownAndInPutIsPositive)
    {
        const auto res = priceBarrier(false, BarrierType::DownAndIn, 80.0);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(BarrierMC, BarrierPriceLessThanVanilla)
    {
        // Any knock-out option ≤ vanilla
        const Real ko = priceBarrier(true, BarrierType::DownAndOut, 80.0).npv;
        const Real vanilla = priceVanillaAnalytic(true).npv;
        EXPECT_LE(ko, vanilla + 0.01);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Barrier far from spot → converges to vanilla
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, FarDownBarrierApproachesVanilla)
    {
        // Barrier at 10 is so far below S0=100 that knock-out almost never triggers
        const Real ko = priceBarrier(true, BarrierType::DownAndOut, 10.0, 0.0, 200000, 1).npv;
        const Real vanilla = priceVanillaAnalytic(true).npv;
        EXPECT_NEAR(ko, vanilla, vanilla * 0.05);
    }

    TEST(BarrierMC, FarUpBarrierKnockInNearZero)
    {
        // Barrier at 300 is so far above S0=100 that an up-and-in rarely triggers
        const Real ki = priceBarrier(true, BarrierType::UpAndIn, 300.0, 0.0, 200000, 1).npv;
        EXPECT_LT(ki, 1.0) << "Up-and-in with very high barrier should be near zero";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Rebate
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, RebateIncreasesKnockOutPrice)
    {
        const Real no_reb = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 200000, 1).npv;
        const Real with_reb = priceBarrier(true, BarrierType::DownAndOut, 80.0, 5.0, 200000, 1).npv;
        EXPECT_GT(with_reb, no_reb) << "Rebate should increase knock-out price";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Reproducibility
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, Reproducibility)
    {
        const auto r1 = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 50000, 99);
        const auto r2 = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 50000, 99);
        EXPECT_DOUBLE_EQ(r1.npv, r2.npv);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Greeks populated
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, GreeksPopulated)
    {
        const auto res = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 100000, 1);
        EXPECT_TRUE(res.greeks.delta.has_value());
        EXPECT_TRUE(res.greeks.gamma.has_value());
        EXPECT_TRUE(res.greeks.vega.has_value());
        EXPECT_TRUE(res.greeks.theta.has_value());
        EXPECT_TRUE(res.greeks.rho.has_value());
    }

    TEST(BarrierMC, DownAndOutCallDeltaPositive)
    {
        const auto res = priceBarrier(true, BarrierType::DownAndOut, 80.0, 0.0, 200000, 1);
        EXPECT_GT(*res.greeks.delta, 0.0) << "Down-and-out call delta should be > 0";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Brownian bridge correction
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, BrownianBridgeLowersThanDiscrete)
    {
        // For knock-out: BB detects more barrier hits → lower price
        const Real bb = priceBarrier(true, BarrierType::DownAndOut, 90.0, 0.0, 200000, 1, true).npv;
        const Real disc = priceBarrier(true, BarrierType::DownAndOut, 90.0, 0.0, 200000, 1, false).npv;
        EXPECT_LE(bb, disc + 0.5)
            << "BB-corrected knock-out should be <= discrete knock-out (more barrier detections)";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Diagnostics string
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, DiagnosticsContainBarrierType)
    {
        auto res = priceBarrier(true, BarrierType::DownAndOut, 80.0);
        EXPECT_NE(res.diagnostics.find("down-and-out"), std::string::npos);

        res = priceBarrier(true, BarrierType::UpAndIn, 120.0);
        EXPECT_NE(res.diagnostics.find("up-and-in"), std::string::npos);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  All four barrier types produce positive price
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BarrierMC, AllBarrierTypesPositive)
    {
        EXPECT_GT(priceBarrier(true, BarrierType::DownAndOut, 80.0).npv, 0.0);
        EXPECT_GT(priceBarrier(true, BarrierType::DownAndIn, 80.0).npv, 0.0);
        EXPECT_GT(priceBarrier(true, BarrierType::UpAndOut, 120.0).npv, 0.0);
        EXPECT_GT(priceBarrier(true, BarrierType::UpAndIn, 120.0).npv, 0.0);
    }

} // namespace quantModeling
