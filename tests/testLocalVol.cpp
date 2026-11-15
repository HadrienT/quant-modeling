#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/models/volatility.hpp"
#include "quantModeling/pricers/adapters/equity_lookback.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla_lv.hpp"
#include "quantModeling/utils/rng.hpp"

#include <cmath>
#include <string>
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

        // Log-Euler is exact for a constant volatility: no bias, only noise.
        EXPECT_NEAR(lv.npv, ana.npv, 4.0 * lv.mc_std_error);
    }

    TEST(LocalVolMC, FlatSurfaceMatchesBSPut)
    {
        auto in = makeFlatLocalVolInput(false, sigma, 30000, 1);
        const auto lv = price_local_vol_mc(in);
        const auto ana = priceAnalytic(false);

        EXPECT_NEAR(lv.npv, ana.npv, 4.0 * lv.mc_std_error);
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
        EXPECT_NE(res.diagnostics.find("local-vol vanilla call, 252 steps"), std::string::npos)
            << res.diagnostics;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  A skewed surface: parity, and an independent hand-written oracle
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {
        // Sticky-strike skew rising into the wings, with a term structure.
        LocalVolInput makeSkewedInput(bool is_call)
        {
            LocalVolInput in = makeFlatLocalVolInput(is_call, sigma, 1 << 17, 7);
            in.strike = 95.0;
            in.compute_greeks = false;
            in.n_steps_per_year = 52;
            in.K_grid = {50.0, 80.0, 100.0, 120.0, 200.0};
            in.T_grid = {0.0, 0.5, 2.0};
            in.sigma_loc_flat = {0.45, 0.40, 0.35,  // K = 50
                                 0.30, 0.28, 0.26,  // K = 80
                                 0.21, 0.20, 0.19,  // K = 100
                                 0.17, 0.17, 0.16,  // K = 120
                                 0.15, 0.15, 0.15}; // K = 200
            return in;
        }

        // The engine retired by WP 26 P3, kept here as the oracle: log-Euler
        // on GridLocalVol, PCG32 + Box-Muller, antithetic pairs.
        std::pair<Real, Real> hand_written_euler(const LocalVolInput &in, int pairs, std::uint64_t seed)
        {
            const GridLocalVol lv(in.K_grid, in.T_grid, in.sigma_loc_flat, 0.0);
            const int steps = std::max(1, static_cast<int>(std::ceil(in.maturity * in.n_steps_per_year)));
            const Real dt = in.maturity / steps, sqdt = std::sqrt(dt), df = std::exp(-in.rate * in.maturity);
            Pcg32 rng(seed, 54);
            NormalBoxMuller gauss;
            Real mean = 0.0, m2 = 0.0;
            for (int p = 0; p < pairs; ++p)
            {
                Real s[2] = {in.spot, in.spot};
                for (int k = 0; k < steps; ++k)
                {
                    const Real z = gauss(rng), t = k * dt;
                    for (int a = 0; a < 2; ++a)
                    {
                        const Real v = lv.value(s[a], t);
                        s[a] *= std::exp((in.rate - in.dividend - 0.5 * v * v) * dt + v * sqdt * (a ? -z : z));
                    }
                }
                Real x = 0.0;
                for (int a = 0; a < 2; ++a)
                    x += 0.5 * df * (in.is_call ? std::max(s[a] - in.strike, 0.0) : std::max(in.strike - s[a], 0.0));
                const Real d = x - mean;
                mean += d / (p + 1);
                m2 += d * (x - mean);
            }
            return {mean, std::sqrt(m2 / (pairs - 1) / pairs)};
        }
    } // namespace

    TEST(LocalVolMC, SkewedSurfaceMatchesTheHandWrittenEuler)
    {
        const LocalVolInput in = makeSkewedInput(false);
        const PricingResult script = price_local_vol_mc(in);
        // The route prices with its steps halved (WP 26 §10.4): the oracle
        // steps on that grid.
        LocalVolInput halved = in;
        halved.n_steps_per_year = 2 * in.n_steps_per_year;
        const auto [oracle, oracle_se] = hand_written_euler(halved, 200000, 11);
        EXPECT_NEAR(script.npv, oracle, 4.0 * std::hypot(script.mc_std_error, oracle_se))
            << "script " << script.npv << " +- " << script.mc_std_error << ", oracle " << oracle << " +- "
            << oracle_se;
    }

    TEST(LocalVolMC, SkewedSurfaceKeepsPutCallParity)
    {
        // Log-Euler with the -v^2/2 drift keeps the discounted spot a
        // martingale step by step, whatever the surface: C - P is the forward.
        const PricingResult call = price_local_vol_mc(makeSkewedInput(true));
        const PricingResult put = price_local_vol_mc(makeSkewedInput(false));
        const Real forward = S0 * std::exp(-q * T) - 95.0 * std::exp(-r * T);
        EXPECT_NEAR(call.npv - put.npv, forward, 4.0 * std::hypot(call.mc_std_error, put.mc_std_error));
    }

    TEST(LocalVolMC, VegaIsTheParallelShiftOfTheGrid)
    {
        // The adjoint vega sums every grid point's sensitivity: the price
        // under the grid shifted by +-0.5 vol point, common Sobol points.
        LocalVolInput in = makeSkewedInput(true);
        in.compute_greeks = true;
        const PricingResult res = price_local_vol_mc(in);
        LocalVolInput up = makeSkewedInput(true), down = makeSkewedInput(true);
        for (Real &v : up.sigma_loc_flat)
            v += 0.005;
        for (Real &v : down.sigma_loc_flat)
            v -= 0.005;
        const Real fd = (price_local_vol_mc(up).npv - price_local_vol_mc(down).npv) / 0.01;
        ASSERT_TRUE(res.greeks.vega.has_value());
        EXPECT_NEAR(*res.greeks.vega, fd, 0.02 * fd);
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
        // Since WP 26 P3 both are scripts, on two simulation models (the
        // Black-Scholes one draws per date, local vol per Euler step, exact
        // for a constant volatility): the same price within error.
        const PricingResult lv = up_and_out_lv({0.01, 2.0}, {0.25, 0.25, 0.25, 0.25});
        const PricingResult bs = up_and_out_bs(0.25);
        EXPECT_NEAR(lv.npv, bs.npv, 4.0 * std::hypot(lv.mc_std_error, bs.mc_std_error));
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

    // ── WP 26 §10.4: a registry price carries its discretisation bias ───

    TEST(LocalVolMC, ThePriceCarriesItsDiscretisationBias)
    {
        // A model that steps on a grid: priced with the steps halved, the
        // bias read from the unhalved run -- small against the price, and
        // the size of the halving's effect on the oracle.
        const LocalVolInput in = makeSkewedInput(false);
        const PricingResult res = price_local_vol_mc(in);
        ASSERT_TRUE(res.discretisation_bias.has_value()) << res.diagnostics;
        EXPECT_GT(*res.discretisation_bias, 0.0);
        EXPECT_LT(*res.discretisation_bias, 0.02 * res.npv) << res.diagnostics;
        EXPECT_NE(res.diagnostics.find("steps halved"), std::string::npos) << res.diagnostics;

        // With greeks, the same price and bias: the differences are taken on
        // the route's own grid, beside the priced run.
        LocalVolInput with_greeks = in;
        with_greeks.compute_greeks = true;
        const PricingResult g = price_local_vol_mc(with_greeks);
        EXPECT_EQ(g.npv, res.npv);
        EXPECT_EQ(*g.discretisation_bias, *res.discretisation_bias);
        EXPECT_TRUE(g.greeks.delta.has_value());
    }

    TEST(LocalVolMC, ARouteExactBetweenItsDatesHasNoBias)
    {
        LookbackBSInput in{};
        in.spot = 100.0;
        in.strike = 100.0;
        in.maturity = 1.0;
        in.rate = 0.03;
        in.dividend = 0.01;
        in.vol = 0.2;
        in.is_call = true;
        in.style = LookbackStyle::FixedStrike;
        in.extremum = LookbackExtremum::Maximum;
        in.n_steps = 12;
        in.n_paths = 1 << 13;
        in.seed = 1;
        const PricingResult res = price_equity_lookback_bs_mc(in);
        ASSERT_TRUE(res.discretisation_bias.has_value()) << res.diagnostics;
        EXPECT_EQ(*res.discretisation_bias, 0.0);
    }

} // namespace quantModeling
