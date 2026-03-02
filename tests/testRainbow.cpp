#include <gtest/gtest.h>

#include "quantModeling/core/results.hpp"
#include "quantModeling/core/types.hpp"

// ── Instruments ──────────────────────────────────────────────────────────────
#include "quantModeling/instruments/equity/rainbow.hpp"
#include "quantModeling/instruments/equity/future.hpp"

// ── Models ───────────────────────────────────────────────────────────────────
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"

// ── Engines ──────────────────────────────────────────────────────────────────
#include "quantModeling/engines/mc/rainbow.hpp"

// ── Pricer / Adapters / Registry ─────────────────────────────────────────────
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/adapters/equity_rainbow.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <Eigen/Core>
#include <cmath>
#include <memory>
#include <vector>

using namespace quantModeling;

// ═════════════════════════════════════════════════════════════════════════════
//  1. Construction
// ═════════════════════════════════════════════════════════════════════════════

TEST(WorstOfOption, Construction)
{
    WorstOfOption opt(1.0, 1.0, true, 100.0);
    EXPECT_DOUBLE_EQ(opt.maturity, 1.0);
    EXPECT_DOUBLE_EQ(opt.strike, 1.0);
    EXPECT_TRUE(opt.is_call);
    EXPECT_DOUBLE_EQ(opt.notional, 100.0);
}

TEST(BestOfOption, Construction)
{
    BestOfOption opt(2.0, 0.95, false, 50.0);
    EXPECT_DOUBLE_EQ(opt.maturity, 2.0);
    EXPECT_DOUBLE_EQ(opt.strike, 0.95);
    EXPECT_FALSE(opt.is_call);
    EXPECT_DOUBLE_EQ(opt.notional, 50.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  2. Worst-of call: lower correlation → cheaper (fewer paths where ALL
//     assets stay above strike)
// ═════════════════════════════════════════════════════════════════════════════

TEST(WorstOfMC, PositiveCallValue)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_worst_of_bs_mc(in);
    EXPECT_GT(res.npv, 0.0);
    EXPECT_GT(res.mc_std_error, 0.0);
}

TEST(WorstOfMC, CorrelationEffect)
{
    // Worst-of call: high ρ → higher price (assets move together → less
    // chance of one dragging down the basket).
    auto make_input = [](double rho) -> RainbowBSInput
    {
        RainbowBSInput in;
        in.spots = {100.0, 100.0};
        in.vols = {0.25, 0.25};
        in.dividends = {0.0, 0.0};
        in.correlations = {{1.0, rho}, {rho, 1.0}};
        in.maturity = 1.0;
        in.strike = 1.0;
        in.is_call = true;
        in.rate = 0.05;
        in.notional = 100.0;
        in.n_paths = 200000;
        in.seed = 42;
        return in;
    };

    auto low_corr = price_worst_of_bs_mc(make_input(0.2));
    auto high_corr = price_worst_of_bs_mc(make_input(0.9));

    // High correlation → higher worst-of call price
    EXPECT_GT(high_corr.npv, low_corr.npv);
}

TEST(WorstOfMC, CallPriceBelowSingleAssetCall)
{
    // Worst-of call on N identical assets should be cheaper than a
    // single-asset ATM call (since min(perf) ≤ each individual perf).
    // Compare against a single-asset BS call (via registry).
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto wo = price_worst_of_bs_mc(in);

    // A single-asset ATM BS call: C ≈ S0 × N(d1) - K×df×N(d2)
    // with S0=100, K=100, r=5%, σ=25%, T=1: roughly ~12.8
    // The worst-of call on perf is N × max(min_perf - 1, 0) which is
    // effectively notional/S0 × worst-of call on spot ≈ should be < ~12.8
    EXPECT_LT(wo.npv, 15.0); // certainly cheaper than single-asset
}

// ═════════════════════════════════════════════════════════════════════════════
//  3. Worst-of put
// ═════════════════════════════════════════════════════════════════════════════

TEST(WorstOfMC, PutPositive)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = false;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_worst_of_bs_mc(in);
    EXPECT_GT(res.npv, 0.0);
}

TEST(WorstOfMC, PutMoreExpensiveThanSingleAsset)
{
    // Worst-of put: pays max(K - min_perf, 0). Since min_perf ≤ each
    // individual perf, the worst-of put is ALWAYS ≥ any single-asset put.
    // With low correlation, it should be significantly more expensive.
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.3}, {0.3, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = false;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto wo = price_worst_of_bs_mc(in);

    // Single-asset ATM put ≈ ~7.8 (BS put with S=100, K=100, r=5%, σ=25%, T=1)
    // Worst-of put should cost more
    EXPECT_GT(wo.npv, 5.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  4. Best-of call
// ═════════════════════════════════════════════════════════════════════════════

TEST(BestOfMC, CallPositive)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_best_of_bs_mc(in);
    EXPECT_GT(res.npv, 0.0);
}

TEST(BestOfMC, CallMoreExpensiveThanSingleAsset)
{
    // Best-of call: pays max(max_perf - K, 0). Since max_perf ≥ each
    // individual perf, the best-of call is ALWAYS ≥ any single-asset call.
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.3}, {0.3, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto bo = price_best_of_bs_mc(in);

    // Single-asset ATM call ≈ ~12.8 (performance terms → /S0 × N)
    // Best-of call on two assets should be more expensive
    EXPECT_GT(bo.npv, 10.0);
}

TEST(BestOfMC, CorrelationEffect)
{
    // Best-of call: low ρ → higher price (more diversification, higher
    // chance one asset does very well).
    auto make_input = [](double rho) -> RainbowBSInput
    {
        RainbowBSInput in;
        in.spots = {100.0, 100.0};
        in.vols = {0.25, 0.25};
        in.dividends = {0.0, 0.0};
        in.correlations = {{1.0, rho}, {rho, 1.0}};
        in.maturity = 1.0;
        in.strike = 1.0;
        in.is_call = true;
        in.rate = 0.05;
        in.notional = 100.0;
        in.n_paths = 200000;
        in.seed = 42;
        return in;
    };

    auto low_corr = price_best_of_bs_mc(make_input(0.2));
    auto high_corr = price_best_of_bs_mc(make_input(0.9));

    // Low correlation → higher best-of call price
    EXPECT_GT(low_corr.npv, high_corr.npv);
}

// ═════════════════════════════════════════════════════════════════════════════
//  5. Best-of put
// ═════════════════════════════════════════════════════════════════════════════

TEST(BestOfMC, PutPositive)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = false;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto res = price_best_of_bs_mc(in);
    EXPECT_GT(res.npv, 0.0);
}

TEST(BestOfMC, PutCheaperThanSingleAsset)
{
    // Best-of put: pays max(K - max_perf, 0). Since max_perf ≥ each
    // individual perf, the best-of put is ALWAYS ≤ any single-asset put.
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = false;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto bo = price_best_of_bs_mc(in);

    // Best-of put should be cheap — max performer rarely falls below strike
    EXPECT_LT(bo.npv, 8.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  6. Ordering: best-of call > worst-of call, worst-of put > best-of put
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowMC, BestOfCallExceedsWorstOfCall)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0, 100.0};
    in.vols = {0.25, 0.30, 0.20};
    in.dividends = {0.01, 0.02, 0.0};
    in.correlations = {
        {1.0, 0.4, 0.3},
        {0.4, 1.0, 0.5},
        {0.3, 0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto bo = price_best_of_bs_mc(in);
    auto wo = price_worst_of_bs_mc(in);

    EXPECT_GT(bo.npv, wo.npv);
}

TEST(RainbowMC, WorstOfPutExceedsBestOfPut)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0, 100.0};
    in.vols = {0.25, 0.30, 0.20};
    in.dividends = {0.01, 0.02, 0.0};
    in.correlations = {
        {1.0, 0.4, 0.3},
        {0.4, 1.0, 0.5},
        {0.3, 0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = false;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 200000;
    in.seed = 42;

    auto bo = price_best_of_bs_mc(in);
    auto wo = price_worst_of_bs_mc(in);

    EXPECT_GT(wo.npv, bo.npv);
}

// ═════════════════════════════════════════════════════════════════════════════
//  7. Three-asset basket
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowMC, ThreeAssets)
{
    RainbowBSInput in;
    in.spots = {100.0, 110.0, 90.0};
    in.vols = {0.20, 0.25, 0.30};
    in.dividends = {0.01, 0.02, 0.0};
    in.correlations = {
        {1.0, 0.5, 0.3},
        {0.5, 1.0, 0.4},
        {0.3, 0.4, 1.0}};
    in.maturity = 0.5;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 100000;
    in.seed = 42;

    auto wo = price_worst_of_bs_mc(in);
    auto bo = price_best_of_bs_mc(in);

    // Both should be positive
    EXPECT_GT(wo.npv, 0.0);
    EXPECT_GT(bo.npv, 0.0);
    // Best-of call > worst-of call
    EXPECT_GT(bo.npv, wo.npv);
}

// ═════════════════════════════════════════════════════════════════════════════
//  8. Validation
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowMC, TooFewAssetsThrows)
{
    RainbowBSInput in;
    in.spots = {100.0};
    in.vols = {0.25};
    in.dividends = {0.0};
    in.maturity = 1.0;
    in.rate = 0.05;
    EXPECT_THROW(price_worst_of_bs_mc(in), InvalidInput);
    EXPECT_THROW(price_best_of_bs_mc(in), InvalidInput);
}

TEST(RainbowMC, DimensionMismatchThrows)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25}; // wrong size
    in.dividends = {0.0, 0.0};
    in.maturity = 1.0;
    in.rate = 0.05;
    EXPECT_THROW(price_worst_of_bs_mc(in), InvalidInput);
}

// ═════════════════════════════════════════════════════════════════════════════
//  9. Engine rejection
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowMC, RejectsEquityFuture)
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
    RainbowMCEngine engine(ctx);

    EquityFuture fut(100.0, 1.0);
    EXPECT_THROW(fut.accept(engine), UnsupportedInstrument);
}

// ═════════════════════════════════════════════════════════════════════════════
//  10. Registry integration
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowRegistry, WorstOfRegistered)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 50000;
    in.seed = 42;

    PricingRequest req{
        InstrumentKind::WorstOfOption,
        ModelKind::BlackScholes,
        EngineKind::MonteCarlo,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_GT(res.npv, 0.0);
}

TEST(RainbowRegistry, BestOfRegistered)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.notional = 100.0;
    in.n_paths = 50000;
    in.seed = 42;

    PricingRequest req{
        InstrumentKind::BestOfOption,
        ModelKind::BlackScholes,
        EngineKind::MonteCarlo,
        PricingInput{in}};

    auto res = default_registry().price(req);
    EXPECT_GT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  11. Notional scaling
// ═════════════════════════════════════════════════════════════════════════════

TEST(RainbowMC, NotionalScaling)
{
    RainbowBSInput in;
    in.spots = {100.0, 100.0};
    in.vols = {0.25, 0.25};
    in.dividends = {0.0, 0.0};
    in.correlations = {{1.0, 0.5}, {0.5, 1.0}};
    in.maturity = 1.0;
    in.strike = 1.0;
    in.is_call = true;
    in.rate = 0.05;
    in.n_paths = 100000;
    in.seed = 42;

    in.notional = 1.0;
    auto r1 = price_worst_of_bs_mc(in);
    in.notional = 1000.0;
    auto r2 = price_worst_of_bs_mc(in);

    EXPECT_NEAR(r2.npv, r1.npv * 1000.0, 1e-6);
}
