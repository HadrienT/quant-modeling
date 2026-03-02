#include <gtest/gtest.h>

#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/analytic/short_rate.hpp"
#include "quantModeling/engines/mc/short_rate.hpp"
#include "quantModeling/instruments/rates/bond_option.hpp"
#include "quantModeling/instruments/rates/capfloor.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/rates/cir.hpp"
#include "quantModeling/models/rates/hull_white.hpp"
#include "quantModeling/models/rates/short_rate_model.hpp"
#include "quantModeling/models/rates/vasicek.hpp"
#include "quantModeling/pricers/adapters/rates_short_rate.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace quantModeling;

// ═════════════════════════════════════════════════════════════════════════════
//  Model construction & parameter access
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateModels, VasicekConstruction)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    EXPECT_DOUBLE_EQ(m.r0(), 0.03);
    EXPECT_DOUBLE_EQ(m.mean_reversion(), 0.5);
    EXPECT_DOUBLE_EQ(m.long_term_rate(), 0.05);
    EXPECT_DOUBLE_EQ(m.volatility(), 0.01);
    EXPECT_EQ(m.model_name(), "VasicekModel");
}

TEST(ShortRateModels, VasicekInvalidParams)
{
    EXPECT_THROW(VasicekModel(-0.1, 0.05, 0.01, 0.03), InvalidInput); // a <= 0
    EXPECT_THROW(VasicekModel(0.5, 0.05, -0.01, 0.03), InvalidInput); // sigma <= 0
}

TEST(ShortRateModels, CIRConstruction)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    EXPECT_DOUBLE_EQ(m.r0(), 0.04);
    EXPECT_DOUBLE_EQ(m.mean_reversion(), 0.5);
    EXPECT_DOUBLE_EQ(m.long_term_rate(), 0.05);
    EXPECT_DOUBLE_EQ(m.volatility(), 0.01);
    EXPECT_EQ(m.model_name(), "CIRModel");
    EXPECT_TRUE(m.feller_satisfied());
}

TEST(ShortRateModels, CIRFellerViolation)
{
    // sigma^2 = 1.0, 2ab = 2*0.5*0.05 = 0.05 < 1.0
    EXPECT_THROW(CIRModel(0.5, 0.05, 1.0, 0.04), InvalidInput);
}

TEST(ShortRateModels, CIRNegativeR0)
{
    EXPECT_THROW(CIRModel(0.5, 0.05, 0.01, -0.01), InvalidInput);
}

TEST(ShortRateModels, HullWhiteConstruction)
{
    HullWhiteModel m(0.1, 0.02, 0.05, 0.005); // theta = 0.005
    EXPECT_DOUBLE_EQ(m.r0(), 0.05);
    EXPECT_DOUBLE_EQ(m.mean_reversion(), 0.1);
    EXPECT_DOUBLE_EQ(m.volatility(), 0.02);
    EXPECT_DOUBLE_EQ(m.theta(), 0.005);
    EXPECT_DOUBLE_EQ(m.long_term_rate(), 0.005 / 0.1); // theta/a
    EXPECT_EQ(m.model_name(), "HullWhiteModel");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Vasicek ZCB pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(VasicekZCB, PriceAtZeroIsOne)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    EXPECT_NEAR(m.zcb_price(0.0), 1.0, 1e-14);
}

TEST(VasicekZCB, ShortMaturity)
{
    // For very short maturity, P ≈ exp(-r0 * T)
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    const Real T = 0.001;
    const Real expected = std::exp(-0.03 * T);
    EXPECT_NEAR(m.zcb_price(T), expected, 1e-6);
}

TEST(VasicekZCB, TypicalPrice)
{
    // Vasicek with a=0.5, b=0.05, sigma=0.01, r0=0.03
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    const Real P = m.zcb_price(5.0);

    // P must be between 0 and 1
    EXPECT_GT(P, 0.0);
    EXPECT_LT(P, 1.0);

    // Cross-check: with low vol and r0 < b, bond is roughly exp(-average_rate * T)
    // Average rate ~ b + (r0-b)*e^{-aT/2} ≈ 0.05 + (0.03-0.05)*something
    // Just check it's reasonable
    EXPECT_NEAR(P, std::exp(-0.05 * 5.0), 0.05); // within 5% of exp(-b*T)
}

TEST(VasicekZCB, PtTMatchesP0T)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    EXPECT_NEAR(m.zcb_price(0.0, 5.0, 0.03), m.zcb_price(5.0), 1e-14);
}

TEST(VasicekZCB, BHelperFunction)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    // B(0) = 0
    EXPECT_NEAR(m.B(0.0), 0.0, 1e-14);
    // B(tau) = (1-exp(-a*tau))/a
    const Real tau = 2.0;
    const Real expected_B = (1.0 - std::exp(-0.5 * 2.0)) / 0.5;
    EXPECT_NEAR(m.B(tau), expected_B, 1e-12);
}

// ═════════════════════════════════════════════════════════════════════════════
//  CIR ZCB pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(CIRZCB, PriceAtZeroIsOne)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    EXPECT_NEAR(m.zcb_price(0.0), 1.0, 1e-14);
}

TEST(CIRZCB, GammaValue)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    const Real gamma = std::sqrt(0.5 * 0.5 + 2.0 * 0.01 * 0.01);
    EXPECT_NEAR(m.gamma(), gamma, 1e-12);
}

TEST(CIRZCB, TypicalPrice)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    const Real P = m.zcb_price(5.0);
    EXPECT_GT(P, 0.0);
    EXPECT_LT(P, 1.0);
}

TEST(CIRZCB, ABConsistency)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    // P(0,T) = A(T)*exp(-B(T)*r0)
    const Real T = 3.0;
    const Real P_direct = m.zcb_price(T);
    const Real P_manual = m.A(T) * std::exp(-m.B(T) * 0.04);
    EXPECT_NEAR(P_direct, P_manual, 1e-12);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Hull-White ZCB pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(HWZCB, EquivalentToVasicek)
{
    // HW with theta = a*b is equivalent to Vasicek
    const Real a = 0.5, b = 0.05, sigma = 0.01, r0 = 0.03;
    VasicekModel vas(a, b, sigma, r0);
    HullWhiteModel hw(a, sigma, r0, a * b);

    for (Real T : {0.5, 1.0, 2.0, 5.0, 10.0})
    {
        EXPECT_NEAR(hw.zcb_price(T), vas.zcb_price(T), 1e-12)
            << "Mismatch at T=" << T;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Vasicek bond option pricing
// ═════════════════════════════════════════════════════════════════════════════

TEST(VasicekBondOption, CallPutParity)
{
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    const Real T = 1.0, S = 5.0, K = 0.8;
    const Real call = m.bond_option_price(true, K, T, S);
    const Real put = m.bond_option_price(false, K, T, S);

    // Put-call parity: Call - Put = P(0,S) - K * P(0,T)
    const Real P0S = m.zcb_price(S);
    const Real P0T = m.zcb_price(T);
    EXPECT_NEAR(call - put, P0S - K * P0T, 1e-10);
}

TEST(VasicekBondOption, CallIsPositive)
{
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    const Real call = m.bond_option_price(true, 0.8, 1.0, 5.0);
    EXPECT_GT(call, 0.0);
}

TEST(VasicekBondOption, PutIsPositive)
{
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    const Real put = m.bond_option_price(false, 0.8, 1.0, 5.0);
    EXPECT_GT(put, 0.0);
}

TEST(VasicekBondOption, ATMOption)
{
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    const Real T = 1.0, S = 5.0;
    const Real K_atm = m.zcb_price(T, S, m.r0()); // forward ZCB price
    // Actually K_atm = P(0,S)/P(0,T) for "forward" ATM
    const Real K_fwd = m.zcb_price(S) / m.zcb_price(T);
    const Real call = m.bond_option_price(true, K_fwd, T, S);
    const Real put = m.bond_option_price(false, K_fwd, T, S);
    // At ATM forward, call ≈ put
    EXPECT_NEAR(call, put, 1e-6);
}

TEST(VasicekBondOption, InvalidInputs)
{
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    EXPECT_THROW(m.bond_option_price(true, 0.8, 0.0, 5.0), InvalidInput);  // T_option = 0
    EXPECT_THROW(m.bond_option_price(true, 0.8, 5.0, 3.0), InvalidInput);  // S <= T
    EXPECT_THROW(m.bond_option_price(true, -0.1, 1.0, 5.0), InvalidInput); // K <= 0
}

// ═════════════════════════════════════════════════════════════════════════════
//  Hull-White bond option — same as Vasicek
// ═════════════════════════════════════════════════════════════════════════════

TEST(HWBondOption, MatchesVasicek)
{
    const Real a = 0.3, b = 0.06, sigma = 0.015, r0 = 0.05;
    VasicekModel vas(a, b, sigma, r0);
    HullWhiteModel hw(a, sigma, r0, a * b);

    const Real T = 1.0, S = 5.0, K = 0.8;
    EXPECT_NEAR(hw.bond_option_price(true, K, T, S),
                vas.bond_option_price(true, K, T, S), 1e-12);
    EXPECT_NEAR(hw.bond_option_price(false, K, T, S),
                vas.bond_option_price(false, K, T, S), 1e-12);
}

// ═════════════════════════════════════════════════════════════════════════════
//  CIR bond option — should throw (no analytic formula)
// ═════════════════════════════════════════════════════════════════════════════

TEST(CIRBondOption, ThrowsForAnalytic)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    EXPECT_THROW(m.bond_option_price(true, 0.8, 1.0, 5.0), UnsupportedInstrument);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Euler step
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateModels, VasicekEulerStep)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    const Real r = 0.04, dt = 0.01, dW = 0.005;
    const Real r_next = m.euler_step(r, 0.0, dt, dW);
    // r + a(b-r)*dt + sigma*dW = 0.04 + 0.5*(0.05-0.04)*0.01 + 0.01*0.005
    const Real expected = 0.04 + 0.5 * (0.05 - 0.04) * 0.01 + 0.01 * 0.005;
    EXPECT_NEAR(r_next, expected, 1e-14);
}

TEST(ShortRateModels, CIREulerStepNonNegative)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    // Large negative shock
    const Real r_next = m.euler_step(0.001, 0.0, 0.01, -10.0);
    EXPECT_GE(r_next, 0.0); // reflection keeps it non-negative
}

TEST(ShortRateModels, HullWhiteEulerStep)
{
    HullWhiteModel m(0.1, 0.02, 0.05, 0.005);
    const Real r = 0.05, dt = 0.01, dW = 0.003;
    const Real r_next = m.euler_step(r, 0.0, dt, dW);
    // r + (theta - a*r)*dt + sigma*dW
    const Real expected = 0.05 + (0.005 - 0.1 * 0.05) * 0.01 + 0.02 * 0.003;
    EXPECT_NEAR(r_next, expected, 1e-14);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Analytic engine — ZCB
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateAnalyticEngine, VasicekZCB)
{
    auto model = std::make_shared<VasicekModel>(0.5, 0.05, 0.01, 0.03);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    ZeroCouponBond bond(5.0, 100.0);
    auto res = price(bond, engine);

    EXPECT_NEAR(res.npv, 100.0 * model->zcb_price(5.0), 1e-10);
    EXPECT_TRUE(res.bond_analytics.macaulay_duration.has_value());
    EXPECT_NEAR(*res.bond_analytics.macaulay_duration, 5.0, 1e-12);
}

TEST(ShortRateAnalyticEngine, CIRZCB)
{
    auto model = std::make_shared<CIRModel>(0.5, 0.05, 0.01, 0.04);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    ZeroCouponBond bond(3.0, 100.0);
    auto res = price(bond, engine);

    EXPECT_NEAR(res.npv, 100.0 * model->zcb_price(3.0), 1e-10);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Analytic engine — FixedRateBond
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateAnalyticEngine, FixedRateBond)
{
    auto model = std::make_shared<VasicekModel>(0.5, 0.05, 0.01, 0.03);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    FixedRateBond bond(0.04, 5.0, 2, 100.0); // 4% semi-annual
    auto res = price(bond, engine);

    // Manual calculation: sum of coupon PVs + principal PV
    const int n = 10; // 5 years * 2
    const Real dt = 0.5;
    const Real coupon = 100.0 * 0.04 * 0.5; // = 2.0
    Real expected = 0.0;
    for (int i = 1; i <= n; ++i)
        expected += coupon * model->zcb_price(dt * static_cast<Real>(i));
    expected += 100.0 * model->zcb_price(5.0);

    EXPECT_NEAR(res.npv, expected, 1e-8);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Analytic engine — BondOption
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateAnalyticEngine, VasicekBondOption)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    BondOption opt(1.0, 5.0, 0.8, true, 100.0);
    auto res = price(opt, engine);

    const Real expected = 100.0 * model->bond_option_price(true, 0.8, 1.0, 5.0);
    EXPECT_NEAR(res.npv, expected, 1e-10);
}

TEST(ShortRateAnalyticEngine, CIRBondOptionThrows)
{
    auto model = std::make_shared<CIRModel>(0.5, 0.05, 0.01, 0.04);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    BondOption opt(1.0, 5.0, 0.8, true);
    EXPECT_THROW(price(opt, engine), UnsupportedInstrument);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Analytic engine — CapFloor
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateAnalyticEngine, VasicekCap)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    // Quarterly cap for 1 year — schedule starts at 0.25 (first reset)
    std::vector<Time> schedule = {0.25, 0.5, 0.75, 1.0, 1.25};
    CapFloor cap(schedule, 0.05, true, 100.0);
    auto res = price(cap, engine);

    EXPECT_GT(res.npv, 0.0);
}

TEST(ShortRateAnalyticEngine, CapFloorParity)
{
    // Cap - Floor = Swap
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};

    std::vector<Time> schedule = {0.5, 1.0, 1.5, 2.0, 2.5};
    const Real K = 0.05;
    const Real N = 100.0;

    ShortRateAnalyticEngine eng_cap(ctx);
    CapFloor cap(schedule, K, true, N);
    auto res_cap = price(cap, eng_cap);

    ShortRateAnalyticEngine eng_floor(ctx);
    CapFloor floor(schedule, K, false, N);
    auto res_floor = price(floor, eng_floor);

    // Cap - Floor = Σ_i N * delta_i * (forward_rate_i - K) * df_i
    // = N * [P(0,T_0) - P(0,T_n) - K * Σ delta_i * P(0,T_i)]
    Real swap_value = model->zcb_price(schedule.front()) - model->zcb_price(schedule.back());
    for (std::size_t i = 0; i < schedule.size() - 1; ++i)
    {
        const Real delta = schedule[i + 1] - schedule[i];
        swap_value -= K * delta * model->zcb_price(schedule[i + 1]);
    }
    swap_value *= N;

    EXPECT_NEAR(res_cap.npv - res_floor.npv, swap_value, 1e-6);
}

// ═════════════════════════════════════════════════════════════════════════════
//  MC engine — ZCB convergence to analytic
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateMCEngine, VasicekZCBConvergence)
{
    auto model = std::make_shared<VasicekModel>(0.5, 0.05, 0.01, 0.03);
    PricingSettings settings;
    settings.mc_paths = 100000;
    settings.mc_seed = 42;
    PricingContext ctx{MarketView{}, settings, model};
    ShortRateMCEngine engine(ctx);

    ZeroCouponBond bond(2.0, 100.0);
    auto res = price(bond, engine);

    const Real analytic = 100.0 * model->zcb_price(2.0);
    // Allow 1% relative error for MC
    EXPECT_NEAR(res.npv, analytic, analytic * 0.01);
    EXPECT_GT(res.mc_std_error, 0.0);
}

TEST(ShortRateMCEngine, CIRZCBConvergence)
{
    auto model = std::make_shared<CIRModel>(0.5, 0.05, 0.01, 0.04);
    PricingSettings settings;
    settings.mc_paths = 100000;
    settings.mc_seed = 42;
    PricingContext ctx{MarketView{}, settings, model};
    ShortRateMCEngine engine(ctx);

    ZeroCouponBond bond(2.0, 100.0);
    auto res = price(bond, engine);

    const Real analytic = 100.0 * model->zcb_price(2.0);
    EXPECT_NEAR(res.npv, analytic, analytic * 0.01);
}

// ═════════════════════════════════════════════════════════════════════════════
//  MC engine — BondOption convergence
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateMCEngine, VasicekBondOptionConvergence)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingSettings settings;
    settings.mc_paths = 200000;
    settings.mc_seed = 123;
    PricingContext ctx{MarketView{}, settings, model};
    ShortRateMCEngine engine(ctx);

    BondOption opt(1.0, 5.0, 0.8, true, 100.0);
    auto res = price(opt, engine);

    const Real analytic = 100.0 * model->bond_option_price(true, 0.8, 1.0, 5.0);
    // MC on bond options is less precise — allow 5% relative error
    EXPECT_NEAR(res.npv, analytic, std::max(analytic * 0.05, 0.001));
}

// ═════════════════════════════════════════════════════════════════════════════
//  MC engine — CIR BondOption (no analytic, just check it runs)
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateMCEngine, CIRBondOptionRuns)
{
    auto model = std::make_shared<CIRModel>(0.5, 0.05, 0.01, 0.04);
    PricingSettings settings;
    settings.mc_paths = 50000;
    settings.mc_seed = 42;
    PricingContext ctx{MarketView{}, settings, model};
    ShortRateMCEngine engine(ctx);

    BondOption opt(1.0, 5.0, 0.8, true, 100.0);
    auto res = price(opt, engine);

    // Just check it produces a non-negative price
    EXPECT_GE(res.npv, 0.0);
    EXPECT_GT(res.mc_std_error, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  MC engine — FixedRateBond
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateMCEngine, VasicekFixedBondConvergence)
{
    auto model = std::make_shared<VasicekModel>(0.5, 0.05, 0.01, 0.03);

    // Analytic
    PricingContext ctx_a{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine eng_a(ctx_a);
    FixedRateBond bond(0.04, 3.0, 1, 100.0);
    auto res_a = price(bond, eng_a);

    // MC
    PricingSettings settings;
    settings.mc_paths = 100000;
    settings.mc_seed = 42;
    PricingContext ctx_mc{MarketView{}, settings, model};
    ShortRateMCEngine eng_mc(ctx_mc);
    FixedRateBond bond2(0.04, 3.0, 1, 100.0);
    auto res_mc = price(bond2, eng_mc);

    EXPECT_NEAR(res_mc.npv, res_a.npv, res_a.npv * 0.02);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Adapter functions
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateAdapter, ZCBVasicek)
{
    ShortRateZCBInput in;
    in.model_type = "vasicek";
    in.a = 0.5;
    in.b = 0.05;
    in.sigma = 0.01;
    in.r0 = 0.03;
    in.maturity = 5.0;
    in.notional = 100.0;

    auto res = price_zcb_short_rate(in);
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    EXPECT_NEAR(res.npv, 100.0 * m.zcb_price(5.0), 1e-10);
}

TEST(ShortRateAdapter, ZCBHullWhite)
{
    ShortRateZCBInput in;
    in.model_type = "hull_white";
    in.a = 0.5;
    in.b = 0.05;
    in.sigma = 0.01;
    in.r0 = 0.03;
    in.maturity = 5.0;
    in.notional = 100.0;

    auto res = price_zcb_short_rate(in);
    HullWhiteModel m(0.5, 0.01, 0.03, 0.5 * 0.05);
    EXPECT_NEAR(res.npv, 100.0 * m.zcb_price(5.0), 1e-10);
}

TEST(ShortRateAdapter, BondOptionAnalytic)
{
    ShortRateBondOptionInput in;
    in.model_type = "vasicek";
    in.a = 0.3;
    in.b = 0.06;
    in.sigma = 0.015;
    in.r0 = 0.05;
    in.option_maturity = 1.0;
    in.bond_maturity = 5.0;
    in.strike = 0.8;
    in.is_call = true;
    in.notional = 100.0;

    auto res = price_bond_option_short_rate_analytic(in);
    VasicekModel m(0.3, 0.06, 0.015, 0.05);
    EXPECT_NEAR(res.npv, 100.0 * m.bond_option_price(true, 0.8, 1.0, 5.0), 1e-10);
}

TEST(ShortRateAdapter, InvalidModelType)
{
    ShortRateZCBInput in;
    in.model_type = "unknown";
    in.a = 0.5;
    in.b = 0.05;
    in.sigma = 0.01;
    in.r0 = 0.03;
    in.maturity = 5.0;

    EXPECT_THROW(price_zcb_short_rate(in), InvalidInput);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Registry integration
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateRegistry, VasicekZCBRegistered)
{
    const auto &reg = default_registry();

    ShortRateZCBInput in;
    in.model_type = "vasicek";
    in.a = 0.5;
    in.b = 0.05;
    in.sigma = 0.01;
    in.r0 = 0.03;
    in.maturity = 5.0;
    in.notional = 100.0;

    PricingRequest req{InstrumentKind::ZeroCouponBond, ModelKind::Vasicek,
                       EngineKind::Analytic, in};
    auto res = reg.price(req);

    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    EXPECT_NEAR(res.npv, 100.0 * m.zcb_price(5.0), 1e-10);
}

TEST(ShortRateRegistry, CIRBondOptionMCRegistered)
{
    const auto &reg = default_registry();

    ShortRateBondOptionInput in;
    in.model_type = "cir";
    in.a = 0.5;
    in.b = 0.05;
    in.sigma = 0.01;
    in.r0 = 0.04;
    in.option_maturity = 1.0;
    in.bond_maturity = 3.0;
    in.strike = 0.9;
    in.is_call = true;
    in.notional = 100.0;
    in.n_paths = 10000;
    in.seed = 42;

    PricingRequest req{InstrumentKind::BondOption, ModelKind::CIR,
                       EngineKind::MonteCarlo, in};
    auto res = reg.price(req);

    EXPECT_GE(res.npv, 0.0);
}

TEST(ShortRateRegistry, CapFloorAnalyticRegistered)
{
    const auto &reg = default_registry();

    ShortRateCapFloorInput in;
    in.model_type = "vasicek";
    in.a = 0.3;
    in.b = 0.06;
    in.sigma = 0.015;
    in.r0 = 0.05;
    in.schedule = {0.25, 0.5, 0.75, 1.0, 1.25};
    in.strike = 0.05;
    in.is_cap = true;
    in.notional = 100.0;

    PricingRequest req{InstrumentKind::CapFloor, ModelKind::Vasicek,
                       EngineKind::Analytic, in};
    auto res = reg.price(req);

    EXPECT_GT(res.npv, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Instrument acceptance (visitor pattern)
// ═════════════════════════════════════════════════════════════════════════════

TEST(ShortRateInstruments, BondOptionAccept)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    BondOption opt(1.0, 5.0, 0.8, true);
    // accept should call visit(const BondOption&)
    opt.accept(engine);
    EXPECT_GT(engine.results().npv, 0.0);
}

TEST(ShortRateInstruments, CapFloorAccept)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    CapFloor cf({0.5, 1.0, 1.5}, 0.05, true);
    cf.accept(engine);
    EXPECT_GT(engine.results().npv, 0.0);
}

TEST(ShortRateInstruments, EquityUnsupported)
{
    auto model = std::make_shared<VasicekModel>(0.3, 0.06, 0.015, 0.05);
    PricingContext ctx{MarketView{}, PricingSettings{}, model};
    ShortRateAnalyticEngine engine(ctx);

    VanillaOption opt(std::make_unique<PlainVanillaPayoff>(OptionType::Call, 100.0),
                      std::make_unique<EuropeanExercise>(1.0));
    EXPECT_THROW(opt.accept(engine), UnsupportedInstrument);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Monotonicity / sanity
// ═════════════════════════════════════════════════════════════════════════════

TEST(VasicekZCB, MonotonicInMaturity)
{
    VasicekModel m(0.5, 0.05, 0.01, 0.03);
    Real prev = 1.0;
    for (Real T : {0.5, 1.0, 2.0, 5.0, 10.0, 20.0})
    {
        const Real P = m.zcb_price(T);
        EXPECT_LT(P, prev) << "ZCB price should decrease with maturity";
        prev = P;
    }
}

TEST(CIRZCB, MonotonicInMaturity)
{
    CIRModel m(0.5, 0.05, 0.01, 0.04);
    Real prev = 1.0;
    for (Real T : {0.5, 1.0, 2.0, 5.0, 10.0, 20.0})
    {
        const Real P = m.zcb_price(T);
        EXPECT_LT(P, prev) << "ZCB price should decrease with maturity";
        prev = P;
    }
}
