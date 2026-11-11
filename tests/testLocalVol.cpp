#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/engines/mc/local_vol.hpp"

#include <cmath>
#include <vector>

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

        // ── Build a flat local-vol surface (should match BS) ────────────────
        LocalVolInput makeFlatLocalVolInput(bool is_call,
                                            Real vol = sigma,
                                            int n_paths = 20000,
                                            int seed = 42)
        {
            LocalVolInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.is_call = is_call;
            in.n_paths = n_paths;
            in.seed = seed;
            in.mc_antithetic = true;
            in.compute_greeks = true;
            in.n_steps_per_year = 252;

            // 2×2 flat grid → bilinear interpolation returns constant vol
            in.K_grid = {50.0, 200.0};
            in.T_grid = {0.01, 2.0};
            in.sigma_loc_flat = {vol, vol, vol, vol}; // 2K × 2T = 4 values

            return in;
        }

        PricingResult priceAnalytic(bool is_call)
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
    //  Flat local-vol surface should match Black-Scholes
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, FlatSurfaceMatchesBSCall)
    {
        auto in = makeFlatLocalVolInput(true, sigma, 30000, 1);
        const auto lv = price_local_vol_mc(in);
        const auto ana = priceAnalytic(true);

        EXPECT_NEAR(lv.npv, ana.npv, ana.npv * 0.08)
            << "Flat local-vol MC call should match BS analytic within 8%";
    }

    TEST(LocalVolMC, FlatSurfaceMatchesBSPut)
    {
        auto in = makeFlatLocalVolInput(false, sigma, 30000, 1);
        const auto lv = price_local_vol_mc(in);
        const auto ana = priceAnalytic(false);

        EXPECT_NEAR(lv.npv, ana.npv, ana.npv * 0.08);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Greeks populated
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, GreeksPopulated)
    {
        auto in = makeFlatLocalVolInput(true);
        const auto res = price_local_vol_mc(in);

        EXPECT_TRUE(res.greeks.delta.has_value());
        EXPECT_TRUE(res.greeks.gamma.has_value());
        EXPECT_TRUE(res.greeks.vega.has_value());
        EXPECT_TRUE(res.greeks.theta.has_value());
        EXPECT_TRUE(res.greeks.rho.has_value());
    }

    TEST(LocalVolMC, CallDeltaPositive)
    {
        auto in = makeFlatLocalVolInput(true, sigma, 10000, 1);
        const auto res = price_local_vol_mc(in);
        EXPECT_GT(*res.greeks.delta, 0.0);
        EXPECT_LT(*res.greeks.delta, 1.0);
    }

    TEST(LocalVolMC, PutDeltaNegative)
    {
        auto in = makeFlatLocalVolInput(false, sigma, 10000, 1);
        const auto res = price_local_vol_mc(in);
        EXPECT_LT(*res.greeks.delta, 0.0);
    }

    TEST(LocalVolMC, VegaPositive)
    {
        auto in = makeFlatLocalVolInput(true, sigma, 10000, 1);
        const auto res = price_local_vol_mc(in);
        EXPECT_GT(*res.greeks.vega, 0.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Reproducibility
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, Reproducibility)
    {
        auto in = makeFlatLocalVolInput(true, sigma, 10000, 99);
        const auto r1 = price_local_vol_mc(in);
        const auto r2 = price_local_vol_mc(in);
        EXPECT_DOUBLE_EQ(r1.npv, r2.npv);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Higher vol surface → higher call price
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, HigherVolIncreasesPrice)
    {
        auto in_lo = makeFlatLocalVolInput(true, 0.15, 10000, 1);
        auto in_hi = makeFlatLocalVolInput(true, 0.30, 10000, 1);
        const Real lo = price_local_vol_mc(in_lo).npv;
        const Real hi = price_local_vol_mc(in_hi).npv;
        EXPECT_LT(lo, hi) << "Higher vol should produce higher ATM call price";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Standard error populated
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, StdErrorPopulated)
    {
        auto in = makeFlatLocalVolInput(true);
        const auto res = price_local_vol_mc(in);
        EXPECT_GT(res.mc_std_error, 0.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  No Greeks mode
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, NoGreeksMode)
    {
        auto in = makeFlatLocalVolInput(true);
        in.compute_greeks = false;
        const auto res = price_local_vol_mc(in);

        EXPECT_GT(res.npv, 0.0);
        EXPECT_FALSE(res.greeks.delta.has_value());
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Diagnostics
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, DiagnosticsContainLocalVol)
    {
        auto in = makeFlatLocalVolInput(true);
        const auto res = price_local_vol_mc(in);
        EXPECT_NE(res.diagnostics.find("LocalVolMC"), std::string::npos);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Validation errors
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, EmptyGridThrows)
    {
        LocalVolInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.K_grid = {};
        in.T_grid = {};
        in.sigma_loc_flat = {};

        EXPECT_THROW(price_local_vol_mc(in), std::invalid_argument);
    }

    TEST(LocalVolMC, MismatchedGridSizeThrows)
    {
        LocalVolInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.K_grid = {80.0, 120.0};
        in.T_grid = {0.5, 1.0};
        in.sigma_loc_flat = {0.2, 0.2}; // should be 4 values

        EXPECT_THROW(price_local_vol_mc(in), std::invalid_argument);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Via registry
    // ─────────────────────────────────────────────────────────────────────────

    TEST(LocalVolMC, RegistryBarrierPath)
    {
        // Only Barrier/Lookback/Asian are registered with DupireLocalVol
        BarrierLocalVolInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.dividend = q;
        in.is_call = true;
        in.barrier_type = BarrierType::DownAndOut;
        in.barrier_level = 80.0;
        in.surface.K_grid = {50.0, 200.0};
        in.surface.T_grid = {0.01, 2.0};
        in.surface.sigma_loc_flat = {0.20, 0.20, 0.20, 0.20};
        in.n_paths = 5000;
        in.seed = 1;
        PricingRequest request{
            InstrumentKind::EquityBarrierOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        const auto res = default_registry().price(request);
        EXPECT_GT(res.npv, 0.0);
    }

    // ── The barrier engine prices on the local-vol surface ────────────────
    // Until 2026-11-10 it priced every model at the flat vol vol_sigma()
    // (sigma_loc(S0, 0) for Dupire): found by the WP 26 P0 benchmark.

    namespace
    {
        PricingResult up_and_out_lv(const std::vector<Real> &T_grid, const std::vector<Real> &sigma_flat)
        {
            BarrierLocalVolInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.is_call = true;
            in.barrier_type = BarrierType::UpAndOut;
            in.barrier_level = 130.0;
            in.n_steps = 52;
            in.brownian_bridge = false;
            in.compute_greeks = false;
            in.surface.K_grid = {50.0, 200.0};
            in.surface.T_grid = T_grid;
            in.surface.sigma_loc_flat = sigma_flat;
            in.n_paths = 100000;
            in.seed = 3;
            return default_registry().price(
                {InstrumentKind::EquityBarrierOption, ModelKind::DupireLocalVol, EngineKind::MonteCarlo, PricingInput{in}});
        }

        PricingResult up_and_out_bs(Real vol)
        {
            BarrierBSInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.vol = vol;
            in.is_call = true;
            in.barrier_type = BarrierType::UpAndOut;
            in.barrier_level = 130.0;
            in.n_steps = 52;
            in.brownian_bridge = false;
            in.compute_greeks = false;
            in.n_paths = 100000;
            in.seed = 3;
            return default_registry().price(
                {InstrumentKind::EquityBarrierOption, ModelKind::BlackScholes, EngineKind::MonteCarlo, PricingInput{in}});
        }
    } // namespace

    TEST(LocalVolMC, BarrierOnAFlatGridIsTheBlackScholesBarrier)
    {
        // Same draws (same seed, same engine): equal up to the rounding of
        // the bilinear interpolation of a constant grid.
        const PricingResult lv = up_and_out_lv({0.01, 2.0}, {0.25, 0.25, 0.25, 0.25});
        const PricingResult bs = up_and_out_bs(0.25);
        EXPECT_NEAR(lv.npv, bs.npv, 1e-10 * bs.npv);
    }

    TEST(LocalVolMC, BarrierSeesTheVolatilityAfterTimeZero)
    {
        // sigma_loc = 20 % at T = 0, 35 % from T = 0.25 on. vol_sigma() reads
        // 20 %; the paths live mostly at 35 %. An up-and-out call loses value
        // with the volatility here (vega < 0), so its price must sit strictly
        // between the flat-35 % and the flat-20 % prices.
        const std::vector<Real> T_grid = {0.0, 0.25, 2.0};
        const PricingResult lv = up_and_out_lv(T_grid, {0.20, 0.35, 0.35, 0.20, 0.35, 0.35});
        const PricingResult low = up_and_out_bs(0.20), high = up_and_out_bs(0.35);
        ASSERT_GT(low.npv, high.npv);
        EXPECT_LT(lv.npv, low.npv - 5.0 * low.mc_std_error);
        EXPECT_GT(lv.npv, high.npv + 5.0 * high.mc_std_error);
    }

} // namespace quantModeling
