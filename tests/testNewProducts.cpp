#include <gtest/gtest.h>

#include "quantModeling/core/results.hpp"
#include "quantModeling/core/types.hpp"

// ── Instruments ──────────────────────────────────────────────────────────────
#include "quantModeling/instruments/equity/variance_swap.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/dispersion.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/instruments/fx/option.hpp"
#include "quantModeling/instruments/commodity/forward.hpp"
#include "quantModeling/instruments/commodity/option.hpp"

// ── Models ───────────────────────────────────────────────────────────────────
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/models/fx/garman_kohlhagen.hpp"
#include "quantModeling/models/commodity/commodity_model.hpp"

// ── Engines ──────────────────────────────────────────────────────────────────
#include "quantModeling/engines/analytic/variance_swap.hpp"
#include "quantModeling/engines/mc/variance_swap.hpp"
#include "quantModeling/engines/mc/dispersion.hpp"
#include "quantModeling/engines/analytic/fx.hpp"
#include "quantModeling/engines/analytic/commodity.hpp"

// ── Pricer / Adapters / Registry ─────────────────────────────────────────────
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/adapters/equity_vol_swap.hpp"
#include "quantModeling/pricers/adapters/equity_dispersion.hpp"
#include "quantModeling/pricers/adapters/fx.hpp"
#include "quantModeling/pricers/adapters/commodity.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <Eigen/Core>
#include <cmath>
#include <memory>
#include <vector>

using namespace quantModeling;

// ═════════════════════════════════════════════════════════════════════════════
//  Helper
// ═════════════════════════════════════════════════════════════════════════════

static inline Real requireGreek(const std::optional<Real> &g)
{
    EXPECT_TRUE(g.has_value());
    return g.value_or(0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  1. Variance Swap — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(VarianceSwap, Construction)
{
    VarianceSwap vs(1.0, 0.04, 100.0);
    EXPECT_DOUBLE_EQ(vs.maturity, 1.0);
    EXPECT_DOUBLE_EQ(vs.strike_var, 0.04);
    EXPECT_DOUBLE_EQ(vs.notional, 100.0);
    EXPECT_TRUE(vs.observation_dates.empty());
}

TEST(VarianceSwap, ConstructionWithDates)
{
    std::vector<Time> dates{0.25, 0.5, 0.75, 1.0};
    VarianceSwap vs(1.0, 0.04, 50.0, dates);
    EXPECT_EQ(vs.observation_dates.size(), 4u);
    EXPECT_DOUBLE_EQ(vs.observation_dates[0], 0.25);
}

// ═════════════════════════════════════════════════════════════════════════════
//  2. Variance Swap — Analytic pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(VarianceSwapAnalytic, FairVarianceEqualsVolSquared)
{
    // Under BS with σ=0.20, fair variance = 0.04.
    // Striking at 0.04 should give zero NPV.
    const Real sigma = 0.20;
    const Real fair_var = sigma * sigma; // 0.04

    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.dividend = 0.02;
    in.vol = sigma;
    in.maturity = 1.0;
    in.strike_var = fair_var;
    in.notional = 100.0;

    auto res = price_variance_swap_bs_analytic(in);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(VarianceSwapAnalytic, PriceFormula)
{
    // NPV = N × (σ² − K_var) × exp(−rT)
    const Real sigma = 0.30;
    const Real K_var = 0.04;
    const Real r = 0.05;
    const Real T = 1.0;
    const Real N = 100.0;

    const Real expected = N * (sigma * sigma - K_var) * std::exp(-r * T);

    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = r;
    in.dividend = 0.0;
    in.vol = sigma;
    in.maturity = T;
    in.strike_var = K_var;
    in.notional = N;

    auto res = price_variance_swap_bs_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-10);
}

TEST(VarianceSwapAnalytic, ZeroMaturityThrows)
{
    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.20;
    in.maturity = 0.0;
    in.strike_var = 0.04;
    EXPECT_THROW(price_variance_swap_bs_analytic(in), InvalidInput);
}

TEST(VarianceSwapAnalytic, NegativeStrike)
{
    // A variance swap with K < σ² should have positive PV (long variance)
    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.30;
    in.maturity = 1.0;
    in.strike_var = 0.0;
    in.notional = 100.0;

    auto res = price_variance_swap_bs_analytic(in);
    EXPECT_GT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  3. Variance Swap — MC pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(VarianceSwapMC, ConvergesToAnalytic)
{
    const Real sigma = 0.25;
    const Real r = 0.05;
    const Real T = 1.0;
    const Real K_var = 0.04;
    const Real N = 100.0;

    const Real analytic = N * (sigma * sigma - K_var) * std::exp(-r * T);

    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = r;
    in.dividend = 0.0;
    in.vol = sigma;
    in.maturity = T;
    in.strike_var = K_var;
    in.notional = N;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_variance_swap_bs_mc(in);
    // MC should converge to analytic within ~3 standard errors
    EXPECT_NEAR(res.npv, analytic, std::max(3.0 * res.mc_std_error, 0.5));
    EXPECT_GT(res.mc_std_error, 0.0);
}

TEST(VarianceSwapMC, FairStrikeGivesZero)
{
    const Real sigma = 0.20;

    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.dividend = 0.0;
    in.vol = sigma;
    in.maturity = 1.0;
    in.strike_var = sigma * sigma;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_variance_swap_bs_mc(in);
    EXPECT_NEAR(res.npv, 0.0, std::max(3.0 * res.mc_std_error, 0.5));
}

// ═════════════════════════════════════════════════════════════════════════════
//  4. Volatility Swap — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(VolatilitySwap, Construction)
{
    VolatilitySwap vs(1.0, 0.20, 100.0);
    EXPECT_DOUBLE_EQ(vs.maturity, 1.0);
    EXPECT_DOUBLE_EQ(vs.strike_vol, 0.20);
    EXPECT_DOUBLE_EQ(vs.notional, 100.0);
    EXPECT_TRUE(vs.observation_dates.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
//  5. Volatility Swap — MC pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(VolatilitySwapMC, JensenInequality)
{
    // Under BS, E[σ_realised] < σ by Jensen's inequality (since sqrt is concave).
    // So vol swap fair strike < σ.
    // If we strike at σ, NPV should be negative (for long vol position).
    const Real sigma = 0.30;

    VolatilitySwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.dividend = 0.0;
    in.vol = sigma;
    in.maturity = 1.0;
    in.strike_vol = sigma;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_volatility_swap_bs_mc(in);
    // E[σ] < σ, so payoff = N × (E[σ] − σ) × df < 0
    EXPECT_LT(res.npv, 0.0);
    EXPECT_GT(res.mc_std_error, 0.0);
}

TEST(VolatilitySwapMC, LowStrikePositive)
{
    // Striking very low (below expected realised vol), long vol swap should have positive PV.
    const Real sigma = 0.30;

    VolatilitySwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.dividend = 0.0;
    in.vol = sigma;
    in.maturity = 1.0;
    in.strike_vol = 0.10; // much below σ
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_volatility_swap_bs_mc(in);
    EXPECT_GT(res.npv, 0.0);
}

TEST(VolatilitySwapMC, ZeroMaturityThrows)
{
    VolatilitySwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.20;
    in.maturity = 0.0;
    in.strike_vol = 0.20;
    EXPECT_THROW(price_volatility_swap_bs_mc(in), InvalidInput);
}

// ═════════════════════════════════════════════════════════════════════════════
//  6. Dispersion Swap — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(DispersionSwap, Construction)
{
    std::vector<Real> weights{0.5, 0.5};
    DispersionSwap ds(1.0, 0.01, weights, 100.0);
    EXPECT_DOUBLE_EQ(ds.maturity, 1.0);
    EXPECT_DOUBLE_EQ(ds.strike_spread, 0.01);
    EXPECT_DOUBLE_EQ(ds.notional, 100.0);
    EXPECT_EQ(ds.weights.size(), 2u);
    EXPECT_TRUE(ds.observation_dates.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
//  7. Dispersion Swap — MC pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(DispersionMC, PositiveDispersionLowCorrelation)
{
    // Two assets with ρ=0.3 should have positive dispersion
    // (weighted sum of individual variances > index variance).
    // K_spread = 0 means we capture pure dispersion.
    DispersionBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.30, 0.30};
    in.dividends = {0.02, 0.02};
    in.weights = {0.5, 0.5};
    in.correlations = {{1.0, 0.3}, {0.3, 1.0}};
    in.maturity = 1.0;
    in.strike_spread = 0.0;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 100000;
    in.seed = 42;

    auto res = price_dispersion_bs_mc(in);
    // With low correlation, dispersion should be positive
    EXPECT_GT(res.npv, 0.0);
    EXPECT_GT(res.mc_std_error, 0.0);
}

TEST(DispersionMC, HighCorrelationLowDispersion)
{
    // With ρ≈1 and identical assets, dispersion should be very small.
    DispersionBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.weights = {0.5, 0.5};
    in.correlations = {{1.0, 0.999}, {0.999, 1.0}};
    in.maturity = 1.0;
    in.strike_spread = 0.0;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 100000;
    in.seed = 42;

    auto res = price_dispersion_bs_mc(in);
    // With near-perfect correlation, dispersion ≈ 0
    EXPECT_NEAR(res.npv, 0.0, std::max(3.0 * res.mc_std_error, 1.0));
}

TEST(DispersionMC, DimensionMismatchThrows)
{
    // Different sizes for spots/weights
    DispersionBSInput in;
    in.spots = {100.0, 100.0, 100.0};
    in.vols = {0.30, 0.30};
    in.dividends = {0.0, 0.0};
    in.weights = {0.5, 0.5};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.rate = 0.05;
    EXPECT_THROW(price_dispersion_bs_mc(in), InvalidInput);
}

TEST(DispersionMC, TooFewAssetsThrows)
{
    DispersionBSInput in;
    in.spots = {100.0};
    in.vols = {0.30};
    in.dividends = {0.0};
    in.weights = {1.0};
    in.maturity = 1.0;
    in.rate = 0.05;
    EXPECT_THROW(price_dispersion_bs_mc(in), InvalidInput);
}

TEST(DispersionMC, ThreeAssetsRuns)
{
    // Verify engine works with >2 assets
    DispersionBSInput in;
    in.spots = {100.0, 110.0, 90.0};
    in.vols = {0.20, 0.25, 0.30};
    in.dividends = {0.01, 0.02, 0.0};
    in.weights = {0.4, 0.3, 0.3};
    in.correlations = {
        {1.0, 0.5, 0.3},
        {0.5, 1.0, 0.4},
        {0.3, 0.4, 1.0}};
    in.maturity = 0.5;
    in.strike_spread = 0.0;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 50000;
    in.seed = 42;

    auto res = price_dispersion_bs_mc(in);
    // With imperfect correlation, expect positive dispersion
    EXPECT_GT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  8. FX Forward — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(FXForward, Construction)
{
    FXForward fwd(1.20, 1.0, 1e6);
    EXPECT_DOUBLE_EQ(fwd.strike, 1.20);
    EXPECT_DOUBLE_EQ(fwd.maturity, 1.0);
    EXPECT_DOUBLE_EQ(fwd.notional, 1e6);
}

// ═════════════════════════════════════════════════════════════════════════════
//  9. FX Forward — Analytic pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(FXForwardAnalytic, CoveredInterestParity)
{
    // F(0,T) = S0 × exp((r_d − r_f) × T)
    // If K = F0, NPV = 0
    const Real S0 = 1.30;  // EUR/USD
    const Real r_d = 0.05; // USD rate
    const Real r_f = 0.03; // EUR rate
    const Real T = 1.0;
    const Real F0 = S0 * std::exp((r_d - r_f) * T);

    FXForwardInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = 0.10;
    in.strike = F0;
    in.maturity = T;
    in.notional = 1.0;

    auto res = price_fx_forward_analytic(in);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(FXForwardAnalytic, PriceFormula)
{
    // PV = N × (F0 − K) × exp(−r_d × T)
    const Real S0 = 1.30;
    const Real r_d = 0.05;
    const Real r_f = 0.03;
    const Real T = 1.0;
    const Real K = 1.25;
    const Real N = 1e6;
    const Real F0 = S0 * std::exp((r_d - r_f) * T);
    const Real expected = N * (F0 - K) * std::exp(-r_d * T);

    FXForwardInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = 0.10;
    in.strike = K;
    in.maturity = T;
    in.notional = N;

    auto res = price_fx_forward_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-6);
}

TEST(FXForwardAnalytic, LongerMaturity)
{
    const Real S0 = 1.10;
    const Real r_d = 0.02;
    const Real r_f = 0.04;
    const Real T = 2.0;
    const Real K = 1.10;
    const Real N = 1.0;
    const Real F0 = S0 * std::exp((r_d - r_f) * T);
    const Real expected = N * (F0 - K) * std::exp(-r_d * T);

    FXForwardInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = 0.12;
    in.strike = K;
    in.maturity = T;
    in.notional = N;

    auto res = price_fx_forward_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-10);
    // r_d < r_f → F0 < S0 → forward is below spot → negative NPV when K = S0
    EXPECT_LT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  10. FX Option — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(FXOption, Construction)
{
    FXOption opt(1.30, 1.0, true, 1e6);
    EXPECT_DOUBLE_EQ(opt.strike, 1.30);
    EXPECT_TRUE(opt.is_call);
    EXPECT_DOUBLE_EQ(opt.notional, 1e6);
}

// ═════════════════════════════════════════════════════════════════════════════
//  11. FX Option — Garman-Kohlhagen pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(FXOptionAnalytic, CallPutParity)
{
    // C − P = S0 × exp(−r_f T) − K × exp(−r_d T)
    const Real S0 = 1.30;
    const Real r_d = 0.05;
    const Real r_f = 0.03;
    const Real T = 1.0;
    const Real K = 1.30;
    const Real sigma = 0.12;
    const Real N = 1.0;

    FXOptionInput call_in;
    call_in.spot = S0;
    call_in.rate_domestic = r_d;
    call_in.rate_foreign = r_f;
    call_in.vol = sigma;
    call_in.strike = K;
    call_in.maturity = T;
    call_in.is_call = true;
    call_in.notional = N;

    FXOptionInput put_in = call_in;
    put_in.is_call = false;

    auto call = price_fx_option_analytic(call_in);
    auto put = price_fx_option_analytic(put_in);

    const Real parity = S0 * std::exp(-r_f * T) - K * std::exp(-r_d * T);
    EXPECT_NEAR(call.npv - put.npv, parity, 1e-10);
}

TEST(FXOptionAnalytic, MatchesBSWithDividend)
{
    // GK is identical to BS with q = r_f.
    // Compare against known BS formula price.
    const Real S0 = 100.0;
    const Real K = 100.0;
    const Real r_d = 0.05;
    const Real r_f = 0.02;
    const Real sigma = 0.20;
    const Real T = 1.0;

    // Manual d1, d2 computation
    const Real d1 = (std::log(S0 / K) + (r_d - r_f + 0.5 * sigma * sigma) * T) /
                    (sigma * std::sqrt(T));
    const Real d2 = d1 - sigma * std::sqrt(T);

    auto norm_cdf_manual = [](Real x) -> Real
    {
        return 0.5 * std::erfc(-x / std::sqrt(2.0));
    };

    const Real df_d = std::exp(-r_d * T);
    const Real df_f = std::exp(-r_f * T);
    const Real expected_call = S0 * df_f * norm_cdf_manual(d1) - K * df_d * norm_cdf_manual(d2);

    FXOptionInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = sigma;
    in.strike = K;
    in.maturity = T;
    in.is_call = true;

    auto res = price_fx_option_analytic(in);
    EXPECT_NEAR(res.npv, expected_call, 1e-10);
}

TEST(FXOptionAnalytic, Greeks)
{
    const Real S0 = 1.30;
    const Real K = 1.30;
    const Real r_d = 0.05;
    const Real r_f = 0.03;
    const Real sigma = 0.12;
    const Real T = 1.0;

    FXOptionInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = sigma;
    in.strike = K;
    in.maturity = T;
    in.is_call = true;

    auto res = price_fx_option_analytic(in);

    // Call delta should be in (0, 1)
    Real delta = requireGreek(res.greeks.delta);
    EXPECT_GT(delta, 0.0);
    EXPECT_LT(delta, 1.0);

    // Gamma should be positive
    Real gamma = requireGreek(res.greeks.gamma);
    EXPECT_GT(gamma, 0.0);

    // Vega should be positive
    Real vega = requireGreek(res.greeks.vega);
    EXPECT_GT(vega, 0.0);

    // Theta should be negative for ATM call
    Real theta = requireGreek(res.greeks.theta);
    EXPECT_LT(theta, 0.0);

    // Rho should be positive for call
    Real rho = requireGreek(res.greeks.rho);
    EXPECT_GT(rho, 0.0);
}

TEST(FXOptionAnalytic, PutGreeks)
{
    FXOptionInput in;
    in.spot = 1.30;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.12;
    in.strike = 1.30;
    in.maturity = 1.0;
    in.is_call = false;

    auto res = price_fx_option_analytic(in);

    // Put delta should be in (−1, 0)
    Real delta = requireGreek(res.greeks.delta);
    EXPECT_LT(delta, 0.0);
    EXPECT_GT(delta, -1.0);

    // Put rho should be negative
    Real rho = requireGreek(res.greeks.rho);
    EXPECT_LT(rho, 0.0);
}

TEST(FXOptionAnalytic, DeepITMCall)
{
    // Deep ITM call should be close to intrinsic value (discounted)
    FXOptionInput in;
    in.spot = 1.50;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.10;
    in.strike = 1.00;
    in.maturity = 0.5;

    auto res = price_fx_option_analytic(in);
    // Deep ITM ≈ S0 × exp(−r_f T) − K × exp(−r_d T)
    const Real intrinsic = 1.50 * std::exp(-0.03 * 0.5) - 1.00 * std::exp(-0.05 * 0.5);
    EXPECT_NEAR(res.npv, intrinsic, 0.01);
}

TEST(FXOptionAnalytic, NotionalScaling)
{
    FXOptionInput in;
    in.spot = 1.30;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.12;
    in.strike = 1.30;
    in.maturity = 1.0;
    in.is_call = true;
    in.notional = 1.0;

    auto r1 = price_fx_option_analytic(in);
    in.notional = 1e6;
    auto r2 = price_fx_option_analytic(in);

    EXPECT_NEAR(r2.npv, r1.npv * 1e6, 1e-4);
}

// ═════════════════════════════════════════════════════════════════════════════
//  12. Commodity Forward — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(CommodityForward, Construction)
{
    CommodityForward fwd(100.0, 1.0, 10.0);
    EXPECT_DOUBLE_EQ(fwd.strike, 100.0);
    EXPECT_DOUBLE_EQ(fwd.maturity, 1.0);
    EXPECT_DOUBLE_EQ(fwd.notional, 10.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  13. Commodity Forward — Analytic pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(CommodityForwardAnalytic, CostOfCarry)
{
    // F = S0 × exp((r + u − y) × T)
    // PV = N × (F − K) × exp(−rT)
    const Real S0 = 80.0;
    const Real r = 0.05;
    const Real u = 0.02; // storage cost
    const Real y = 0.03; // convenience yield
    const Real T = 1.0;
    const Real K = 82.0;
    const Real N = 10.0;

    const Real F = S0 * std::exp((r + u - y) * T);
    const Real expected = N * (F - K) * std::exp(-r * T);

    CommodityForwardInput in;
    in.spot = S0;
    in.rate = r;
    in.storage_cost = u;
    in.convenience_yield = y;
    in.vol = 0.20;
    in.strike = K;
    in.maturity = T;
    in.notional = N;

    auto res = price_commodity_forward_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-10);
}

TEST(CommodityForwardAnalytic, FairForwardZeroNPV)
{
    const Real S0 = 80.0;
    const Real r = 0.05;
    const Real u = 0.02;
    const Real y = 0.03;
    const Real T = 1.0;
    const Real F = S0 * std::exp((r + u - y) * T);

    CommodityForwardInput in;
    in.spot = S0;
    in.rate = r;
    in.storage_cost = u;
    in.convenience_yield = y;
    in.vol = 0.20;
    in.strike = F; // strike = forward → NPV = 0
    in.maturity = T;

    auto res = price_commodity_forward_analytic(in);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(CommodityForwardAnalytic, NoStorageNoCY)
{
    // With u=y=0, reduces to F = S0 × exp(rT): normal equity forward
    const Real S0 = 100.0;
    const Real r = 0.05;
    const Real T = 1.0;
    const Real K = 100.0;
    const Real F = S0 * std::exp(r * T);
    const Real expected = (F - K) * std::exp(-r * T);

    CommodityForwardInput in;
    in.spot = S0;
    in.rate = r;
    in.storage_cost = 0.0;
    in.convenience_yield = 0.0;
    in.vol = 0.20;
    in.strike = K;
    in.maturity = T;

    auto res = price_commodity_forward_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-10);
}

// ═════════════════════════════════════════════════════════════════════════════
//  14. Commodity Option — construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(CommodityOption, Construction)
{
    CommodityOption opt(100.0, 1.0, true, 10.0);
    EXPECT_DOUBLE_EQ(opt.strike, 100.0);
    EXPECT_TRUE(opt.is_call);
    EXPECT_DOUBLE_EQ(opt.notional, 10.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  15. Commodity Option — Black '76 pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(CommodityOptionAnalytic, CallPutParity)
{
    // C − P = df × (F − K)
    const Real S0 = 80.0;
    const Real r = 0.05;
    const Real u = 0.02;
    const Real y = 0.03;
    const Real T = 1.0;
    const Real K = 82.0;
    const Real sigma = 0.25;
    const Real F = S0 * std::exp((r + u - y) * T);
    const Real df = std::exp(-r * T);

    CommodityOptionInput call_in;
    call_in.spot = S0;
    call_in.rate = r;
    call_in.storage_cost = u;
    call_in.convenience_yield = y;
    call_in.vol = sigma;
    call_in.strike = K;
    call_in.maturity = T;
    call_in.is_call = true;

    CommodityOptionInput put_in = call_in;
    put_in.is_call = false;

    auto call = price_commodity_option_analytic(call_in);
    auto put = price_commodity_option_analytic(put_in);

    const Real parity = df * (F - K);
    EXPECT_NEAR(call.npv - put.npv, parity, 1e-10);
}

TEST(CommodityOptionAnalytic, PriceFormula)
{
    // Verify against manual Black '76 computation
    const Real S0 = 100.0;
    const Real r = 0.05;
    const Real u = 0.0;
    const Real y = 0.0;
    const Real sigma = 0.20;
    const Real T = 1.0;
    const Real K = 100.0;
    const Real F = S0 * std::exp(r * T);
    const Real df = std::exp(-r * T);

    auto N_cdf = [](Real x) -> Real
    {
        return 0.5 * std::erfc(-x / std::sqrt(2.0));
    };

    const Real d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * std::sqrt(T));
    const Real d2 = d1 - sigma * std::sqrt(T);
    const Real expected = df * (F * N_cdf(d1) - K * N_cdf(d2));

    CommodityOptionInput in;
    in.spot = S0;
    in.rate = r;
    in.storage_cost = u;
    in.convenience_yield = y;
    in.vol = sigma;
    in.strike = K;
    in.maturity = T;
    in.is_call = true;

    auto res = price_commodity_option_analytic(in);
    EXPECT_NEAR(res.npv, expected, 1e-10);
}

TEST(CommodityOptionAnalytic, Greeks)
{
    CommodityOptionInput in;
    in.spot = 80.0;
    in.rate = 0.05;
    in.storage_cost = 0.02;
    in.convenience_yield = 0.03;
    in.vol = 0.25;
    in.strike = 82.0;
    in.maturity = 1.0;
    in.is_call = true;

    auto res = price_commodity_option_analytic(in);

    // Call delta should be in (0, 1) (Black '76 delta = df × N(d1))
    Real delta = requireGreek(res.greeks.delta);
    EXPECT_GT(delta, 0.0);
    EXPECT_LT(delta, 1.0);

    // Gamma positive
    Real gamma = requireGreek(res.greeks.gamma);
    EXPECT_GT(gamma, 0.0);

    // Vega positive
    Real vega = requireGreek(res.greeks.vega);
    EXPECT_GT(vega, 0.0);
}

TEST(CommodityOptionAnalytic, PutGreeks)
{
    CommodityOptionInput in;
    in.spot = 80.0;
    in.rate = 0.05;
    in.storage_cost = 0.02;
    in.convenience_yield = 0.03;
    in.vol = 0.25;
    in.strike = 82.0;
    in.maturity = 1.0;
    in.is_call = false;

    auto res = price_commodity_option_analytic(in);

    // Put delta should be in (−1, 0)
    Real delta = requireGreek(res.greeks.delta);
    EXPECT_LT(delta, 0.0);
    EXPECT_GT(delta, -1.0);
}

TEST(CommodityOptionAnalytic, DeepITMCallApproachesForward)
{
    // Deep ITM call ≈ intrinsic = df × (F − K)
    CommodityOptionInput in;
    in.spot = 150.0;
    in.rate = 0.05;
    in.storage_cost = 0.0;
    in.convenience_yield = 0.0;
    in.vol = 0.10;
    in.strike = 50.0;
    in.maturity = 0.5;
    in.is_call = true;

    const Real F = 150.0 * std::exp(0.05 * 0.5);
    const Real df = std::exp(-0.05 * 0.5);
    const Real intrinsic = df * (F - 50.0);

    auto res = price_commodity_option_analytic(in);
    EXPECT_NEAR(res.npv, intrinsic, 0.1);
}

// ═════════════════════════════════════════════════════════════════════════════
//  16. Model — GarmanKohlhagenModel
// ═════════════════════════════════════════════════════════════════════════════

TEST(GarmanKohlhagenModel, Interface)
{
    GarmanKohlhagenModel gk(1.30, 0.05, 0.03, 0.12);
    EXPECT_DOUBLE_EQ(gk.spot0(), 1.30);
    EXPECT_DOUBLE_EQ(gk.rate_r(), 0.05);
    EXPECT_DOUBLE_EQ(gk.yield_q(), 0.03);
    EXPECT_DOUBLE_EQ(gk.vol_sigma(), 0.12);
    EXPECT_EQ(gk.model_name(), "GarmanKohlhagenModel");
}

// ═════════════════════════════════════════════════════════════════════════════
//  17. Model — CommodityBlackModel
// ═════════════════════════════════════════════════════════════════════════════

TEST(CommodityBlackModel, Interface)
{
    CommodityBlackModel m(80.0, 0.05, 0.02, 0.03, 0.25);
    EXPECT_DOUBLE_EQ(m.spot(), 80.0);
    EXPECT_DOUBLE_EQ(m.rate(), 0.05);
    EXPECT_DOUBLE_EQ(m.storage_cost(), 0.02);
    EXPECT_DOUBLE_EQ(m.convenience_yield(), 0.03);
    EXPECT_DOUBLE_EQ(m.vol_sigma(), 0.25);
    EXPECT_EQ(m.model_name(), "CommodityBlackModel");
}

TEST(CommodityBlackModel, CostOfCarry)
{
    CommodityBlackModel m(80.0, 0.05, 0.02, 0.03, 0.25);
    EXPECT_DOUBLE_EQ(m.carry(), 0.04); // 0.05 + 0.02 - 0.03
}

TEST(CommodityBlackModel, ForwardPrice)
{
    CommodityBlackModel m(80.0, 0.05, 0.02, 0.03, 0.25);
    const Real T = 1.0;
    const Real expected = 80.0 * std::exp(0.04 * T);
    EXPECT_NEAR(m.forward(T), expected, 1e-12);
}

// ═════════════════════════════════════════════════════════════════════════════
//  18. Visitor rejection tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(EngineRejection, VarianceSwapAnalyticRejectsFuture)
{
    auto model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    VarianceSwapAnalyticEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

TEST(EngineRejection, VolSwapMCRejectsFuture)
{
    auto model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
    PricingSettings s;
    s.mc_paths = 1000;
    s.mc_seed = 1;
    PricingContext ctx{MarketView{}, s, model};
    VolSwapMCEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

TEST(EngineRejection, DispersionMCRejectsFuture)
{
    Eigen::MatrixXd corr = Eigen::MatrixXd::Identity(2, 2);
    auto model = std::make_shared<MultiAssetBSModel>(
        0.05, std::vector<Real>{100.0, 100.0},
        std::vector<Real>{0.20, 0.20},
        std::vector<Real>{0.0, 0.0}, corr);
    PricingSettings s;
    s.mc_paths = 1000;
    s.mc_seed = 1;
    PricingContext ctx{MarketView{}, s, model};
    DispersionMCEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

TEST(EngineRejection, FXAnalyticRejectsFuture)
{
    auto model = std::make_shared<GarmanKohlhagenModel>(1.30, 0.05, 0.03, 0.12);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    FXAnalyticEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

TEST(EngineRejection, CommodityAnalyticRejectsFuture)
{
    auto model = std::make_shared<CommodityBlackModel>(80.0, 0.05, 0.02, 0.03, 0.25);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    CommodityAnalyticEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

// ═════════════════════════════════════════════════════════════════════════════
//  19. Registry integration tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(RegistryNewProducts, VarianceSwapAnalytic)
{
    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.20;
    in.maturity = 1.0;
    in.strike_var = 0.04;
    in.notional = 100.0;

    PricingRequest req{
        InstrumentKind::VarianceSwap,
        ModelKind::BlackScholes,
        EngineKind::Analytic,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(RegistryNewProducts, VarianceSwapMC)
{
    VarianceSwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.25;
    in.maturity = 1.0;
    in.strike_var = 0.04;
    in.notional = 100.0;
    in.n_paths = 100000;
    in.seed = 42;

    PricingRequest req{
        InstrumentKind::VarianceSwap,
        ModelKind::BlackScholes,
        EngineKind::MonteCarlo,
        PricingInput{in}};

    auto res = default_registry().price(req);
    const Real analytic = 100.0 * (0.0625 - 0.04) * std::exp(-0.05);
    EXPECT_NEAR(res.npv, analytic, std::max(3.0 * res.mc_std_error, 0.5));
}

TEST(RegistryNewProducts, VolatilitySwapMC)
{
    VolatilitySwapBSInput in;
    in.spot = 100.0;
    in.rate = 0.05;
    in.vol = 0.20;
    in.maturity = 1.0;
    in.strike_vol = 0.10;
    in.notional = 100.0;
    in.n_paths = 100000;
    in.seed = 42;

    PricingRequest req{
        InstrumentKind::VolatilitySwap,
        ModelKind::BlackScholes,
        EngineKind::MonteCarlo,
        PricingInput{in}};

    auto res = default_registry().price(req);
    // Striking below fair vol → positive
    EXPECT_GT(res.npv, 0.0);
}

TEST(RegistryNewProducts, DispersionSwapMC)
{
    DispersionBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.30, 0.30};
    in.dividends = {0.0, 0.0};
    in.weights = {0.5, 0.5};
    in.correlations = {{1.0, 0.3}, {0.3, 1.0}};
    in.maturity = 1.0;
    in.strike_spread = 0.0;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 50000;
    in.seed = 42;

    PricingRequest req{
        InstrumentKind::DispersionSwap,
        ModelKind::BlackScholes,
        EngineKind::MonteCarlo,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_GT(res.npv, 0.0);
}

TEST(RegistryNewProducts, FXForwardAnalytic)
{
    const Real S0 = 1.30;
    const Real r_d = 0.05;
    const Real r_f = 0.03;
    const Real T = 1.0;
    const Real F0 = S0 * std::exp((r_d - r_f) * T);

    FXForwardInput in;
    in.spot = S0;
    in.rate_domestic = r_d;
    in.rate_foreign = r_f;
    in.vol = 0.12;
    in.strike = F0;
    in.maturity = T;

    PricingRequest req{
        InstrumentKind::FXForward,
        ModelKind::GarmanKohlhagen,
        EngineKind::Analytic,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(RegistryNewProducts, FXOptionAnalytic)
{
    FXOptionInput in;
    in.spot = 1.30;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.12;
    in.strike = 1.30;
    in.maturity = 1.0;
    in.is_call = true;

    PricingRequest req{
        InstrumentKind::FXOption,
        ModelKind::GarmanKohlhagen,
        EngineKind::Analytic,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_GT(res.npv, 0.0); // ATM call has positive premium
}

TEST(RegistryNewProducts, CommodityForwardAnalytic)
{
    const Real S0 = 80.0;
    const Real r = 0.05;
    const Real u = 0.02;
    const Real y = 0.03;
    const Real T = 1.0;
    const Real F = S0 * std::exp((r + u - y) * T);

    CommodityForwardInput in;
    in.spot = S0;
    in.rate = r;
    in.storage_cost = u;
    in.convenience_yield = y;
    in.vol = 0.25;
    in.strike = F;
    in.maturity = T;

    PricingRequest req{
        InstrumentKind::CommodityForward,
        ModelKind::CommodityBlack,
        EngineKind::Analytic,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_NEAR(res.npv, 0.0, 1e-12);
}

TEST(RegistryNewProducts, CommodityOptionAnalytic)
{
    CommodityOptionInput in;
    in.spot = 80.0;
    in.rate = 0.05;
    in.storage_cost = 0.02;
    in.convenience_yield = 0.03;
    in.vol = 0.25;
    in.strike = 82.0;
    in.maturity = 1.0;
    in.is_call = true;

    PricingRequest req{
        InstrumentKind::CommodityOption,
        ModelKind::CommodityBlack,
        EngineKind::Analytic,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_GT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  20. Input validation edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(FXForwardAnalytic, ZeroMaturityThrows)
{
    FXForwardInput in;
    in.spot = 1.30;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.12;
    in.strike = 1.30;
    in.maturity = 0.0;
    EXPECT_THROW(price_fx_forward_analytic(in), InvalidInput);
}

TEST(FXOptionAnalytic, ZeroMaturityThrows)
{
    FXOptionInput in;
    in.spot = 1.30;
    in.rate_domestic = 0.05;
    in.rate_foreign = 0.03;
    in.vol = 0.12;
    in.strike = 1.30;
    in.maturity = 0.0;
    in.is_call = true;
    EXPECT_THROW(price_fx_option_analytic(in), InvalidInput);
}

TEST(CommodityForwardAnalytic, ZeroMaturityThrows)
{
    CommodityForwardInput in;
    in.spot = 80.0;
    in.rate = 0.05;
    in.vol = 0.25;
    in.strike = 80.0;
    in.maturity = 0.0;
    EXPECT_THROW(price_commodity_forward_analytic(in), InvalidInput);
}

TEST(CommodityOptionAnalytic, ZeroMaturityThrows)
{
    CommodityOptionInput in;
    in.spot = 80.0;
    in.rate = 0.05;
    in.vol = 0.25;
    in.strike = 80.0;
    in.maturity = 0.0;
    in.is_call = true;
    EXPECT_THROW(price_commodity_option_analytic(in), InvalidInput);
}
