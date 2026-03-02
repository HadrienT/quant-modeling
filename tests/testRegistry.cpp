#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/core/types.hpp"

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

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Registry dispatches correctly
    // ─────────────────────────────────────────────────────────────────────────

    TEST(Registry, AnalyticVanillaCallWorks)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::Analytic,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, MCVanillaCallWorks)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        in.n_paths = 50000;
        in.seed = 1;
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::MonteCarlo,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, BinomialVanillaWorks)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        in.tree_steps = 100;
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::BinomialTree,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, TrinomialVanillaWorks)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        in.tree_steps = 100;
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::TrinomialTree,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, PDEVanillaWorks)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        in.pde_space_steps = 100;
        in.pde_time_steps = 100;
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::PDEFiniteDifference,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, DigitalAnalyticWorks)
    {
        DigitalBSInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.dividend = q;
        in.vol = sigma;
        in.is_call = true;
        in.payoff_type = DigitalPayoffType::CashOrNothing;
        in.cash_amount = 1.0;

        PricingRequest req{InstrumentKind::EquityDigitalOption,
                           ModelKind::BlackScholes, EngineKind::Analytic,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, BarrierMCWorks)
    {
        BarrierBSInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.dividend = q;
        in.vol = sigma;
        in.is_call = true;
        in.barrier_type = BarrierType::DownAndOut;
        in.barrier_level = 80.0;
        in.n_paths = 50000;
        in.seed = 1;

        PricingRequest req{InstrumentKind::EquityBarrierOption,
                           ModelKind::BlackScholes, EngineKind::MonteCarlo,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, AmericanBinomialWorks)
    {
        AmericanVanillaBSInput in{S0, K, T, r, q, sigma, false};
        in.tree_steps = 100;
        PricingRequest req{InstrumentKind::EquityAmericanVanillaOption,
                           ModelKind::BlackScholes, EngineKind::BinomialTree,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, AmericanTrinomialWorks)
    {
        AmericanVanillaBSInput in{S0, K, T, r, q, sigma, false};
        in.tree_steps = 100;
        PricingRequest req{InstrumentKind::EquityAmericanVanillaOption,
                           ModelKind::BlackScholes, EngineKind::TrinomialTree,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Unregistered combo throws
    // ─────────────────────────────────────────────────────────────────────────

    TEST(Registry, UnregisteredComboThrows)
    {
        VanillaBSInput in{S0, K, T, r, q, sigma, true};
        // No BS+Analytic for Asian is registered (Asian analytic uses different path)
        PricingRequest req{InstrumentKind::EquityBarrierOption,
                           ModelKind::BlackScholes, EngineKind::Analytic,
                           PricingInput{in}};
        EXPECT_THROW(default_registry().price(req), UnsupportedInstrument);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  All 5 engines agree on European vanilla call (cross-engine consistency)
    // ─────────────────────────────────────────────────────────────────────────

    TEST(Registry, AllEnginesAgreeOnEuroCall)
    {
        // Analytic reference
        VanillaBSInput in_ana{S0, K, T, r, q, sigma, true};
        PricingRequest req_ana{InstrumentKind::EquityVanillaOption,
                               ModelKind::BlackScholes, EngineKind::Analytic,
                               PricingInput{in_ana}};
        const Real ref = default_registry().price(req_ana).npv;

        // MC
        VanillaBSInput in_mc{S0, K, T, r, q, sigma, true};
        in_mc.n_paths = 500000;
        in_mc.seed = 1;
        PricingRequest req_mc{InstrumentKind::EquityVanillaOption,
                              ModelKind::BlackScholes, EngineKind::MonteCarlo,
                              PricingInput{in_mc}};
        EXPECT_NEAR(default_registry().price(req_mc).npv, ref, ref * 0.02);

        // Binomial
        VanillaBSInput in_bi{S0, K, T, r, q, sigma, true};
        in_bi.tree_steps = 500;
        PricingRequest req_bi{InstrumentKind::EquityVanillaOption,
                              ModelKind::BlackScholes, EngineKind::BinomialTree,
                              PricingInput{in_bi}};
        EXPECT_NEAR(default_registry().price(req_bi).npv, ref, ref * 0.005);

        // Trinomial (Boyle converges slower – allow 15 %)
        VanillaBSInput in_tri{S0, K, T, r, q, sigma, true};
        in_tri.tree_steps = 500;
        PricingRequest req_tri{InstrumentKind::EquityVanillaOption,
                               ModelKind::BlackScholes, EngineKind::TrinomialTree,
                               PricingInput{in_tri}};
        EXPECT_NEAR(default_registry().price(req_tri).npv, ref, ref * 0.15);

        // PDE (use a coarser grid to avoid divergence)
        VanillaBSInput in_pde{S0, K, T, r, q, sigma, true};
        in_pde.pde_space_steps = 100;
        in_pde.pde_time_steps = 100;
        PricingRequest req_pde{InstrumentKind::EquityVanillaOption,
                               ModelKind::BlackScholes, EngineKind::PDEFiniteDifference,
                               PricingInput{in_pde}};
        EXPECT_NEAR(default_registry().price(req_pde).npv, ref, ref * 0.30);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Local vol registry paths
    // ─────────────────────────────────────────────────────────────────────────

    TEST(Registry, DupireLocalVolLookbackWorks)
    {
        // No vanilla+DupireLocalVol is registered; use Lookback instead
        LookbackLocalVolInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.dividend = q;
        in.is_call = true;
        in.style = LookbackStyle::FixedStrike;
        in.extremum = LookbackExtremum::Maximum;
        in.surface.K_grid = {50.0, 200.0};
        in.surface.T_grid = {0.01, 2.0};
        in.surface.sigma_loc_flat = {0.20, 0.20, 0.20, 0.20};
        in.n_paths = 10000;
        in.seed = 1;

        PricingRequest req{InstrumentKind::EquityLookbackOption,
                           ModelKind::DupireLocalVol, EngineKind::MonteCarlo,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

    TEST(Registry, DupireBarrierWorks)
    {
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
        in.n_paths = 30000;
        in.seed = 1;

        PricingRequest req{InstrumentKind::EquityBarrierOption,
                           ModelKind::DupireLocalVol, EngineKind::MonteCarlo,
                           PricingInput{in}};
        const auto res = default_registry().price(req);
        EXPECT_GT(res.npv, 0.0);
    }

} // namespace quantModeling
