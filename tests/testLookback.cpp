#include <gtest/gtest.h>

#include "quantModeling/engines/mc/lookback.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    namespace
    {

        constexpr Real S0 = 100.0;
        constexpr Real K = 100.0; // ATM
        constexpr Real T = 1.0;
        constexpr Real r = 0.05;
        constexpr Real q = 0.02;
        constexpr Real sigma = 0.20;

        // ---- Helpers -------------------------------------------------------

        PricingResult price_vanilla(bool is_call)
        {
            VanillaBSInput in{S0, K, T, r, q, sigma, is_call};
            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes,
                EngineKind::Analytic,
                PricingInput{in}};
            return default_registry().price(request);
        }

        PricingResult price_lookback(
            bool is_call,
            LookbackStyle style,
            LookbackExtremum extremum,
            int n_paths = 200000, int seed = 42)
        {
            LookbackBSInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.vol = sigma;
            in.is_call = is_call;
            in.style = style;
            in.extremum = extremum;
            in.n_steps = 0;
            in.n_paths = n_paths;
            in.seed = seed;

            PricingRequest request{
                InstrumentKind::EquityLookbackOption,
                ModelKind::BlackScholes,
                EngineKind::MonteCarlo,
                PricingInput{in}};
            return default_registry().price(request);
        }

    } // namespace

    // ---- Fixed-strike sanity bounds ----------------------------------------

    // Fixed-strike lookback call on the maximum always pays at least as much as
    // a vanilla call with the same strike, because max(S) >= S_T.
    TEST(LookbackMC, FixedStrikeCallMaxIsAboveVanillaCall)
    {
        const PricingResult vanilla = price_vanilla(true);
        const PricingResult lookback = price_lookback(
            true, LookbackStyle::FixedStrike, LookbackExtremum::Maximum);

        EXPECT_GT(lookback.npv, vanilla.npv);
        EXPECT_GT(lookback.mc_std_error, 0.0);
    }

    // Fixed-strike lookback put on the minimum always pays at least as much as
    // a vanilla put with the same strike, because min(S) <= S_T.
    TEST(LookbackMC, FixedStrikePutMinIsAboveVanillaPut)
    {
        const PricingResult vanilla = price_vanilla(false);
        const PricingResult lookback = price_lookback(
            false, LookbackStyle::FixedStrike, LookbackExtremum::Minimum);

        EXPECT_GT(lookback.npv, vanilla.npv);
        EXPECT_GT(lookback.mc_std_error, 0.0);
    }

    // ---- Floating-strike correctness ---------------------------------------

    // Float call pays S_T - S_min >= 0, and since S_min <= S0 by construction
    // (initialised at S0, path can only move down from there), float call >= vanilla ATM call.
    TEST(LookbackMC, FloatingStrikeCallAboveVanillaCall)
    {
        const PricingResult vanilla = price_vanilla(true);
        const PricingResult lookback = price_lookback(
            true, LookbackStyle::FloatingStrike, LookbackExtremum::Minimum);

        EXPECT_GT(lookback.npv, vanilla.npv);
        EXPECT_GT(lookback.mc_std_error, 0.0);
    }

    // Float put pays S_max - S_T >= 0, and since S_max >= S0, float put >= vanilla ATM put.
    TEST(LookbackMC, FloatingStrikePutAboveVanillaPut)
    {
        const PricingResult vanilla = price_vanilla(false);
        const PricingResult lookback = price_lookback(
            false, LookbackStyle::FloatingStrike, LookbackExtremum::Maximum);

        EXPECT_GT(lookback.npv, vanilla.npv);
        EXPECT_GT(lookback.mc_std_error, 0.0);
    }

    // The engine must derive the correct extremum for floating-strike options
    // regardless of the extremum field value. A floating call priced with
    // extremum=Minimum and extremum=Maximum should give the same result because
    // the engine ignores the field for FloatingStrike.
    TEST(LookbackMC, FloatingStrikeIgnoresExtremumField)
    {
        const PricingResult p_min = price_lookback(
            true, LookbackStyle::FloatingStrike, LookbackExtremum::Minimum, 300000, 7);
        const PricingResult p_max = price_lookback(
            true, LookbackStyle::FloatingStrike, LookbackExtremum::Maximum, 300000, 7);

        // Same seed => identical paths => price must be bit-exact.
        EXPECT_NEAR(p_min.npv, p_max.npv, 1e-12);
    }

    // ---- Greeks are populated ----------------------------------------------

    TEST(LookbackMC, GreeksArePopulated)
    {
        const PricingResult res = price_lookback(
            true, LookbackStyle::FixedStrike, LookbackExtremum::Maximum);

        ASSERT_TRUE(res.greeks.delta.has_value());
        ASSERT_TRUE(res.greeks.gamma.has_value());
        ASSERT_TRUE(res.greeks.vega.has_value());
        ASSERT_TRUE(res.greeks.rho.has_value());
        ASSERT_TRUE(res.greeks.theta.has_value());

        // Delta of a call-on-max must be positive.
        EXPECT_GT(*res.greeks.delta, 0.0);

        // Std errors must be non-negative.
        EXPECT_GE(*res.greeks.delta_std_error, 0.0);
        EXPECT_GE(*res.greeks.vega_std_error, 0.0);
        EXPECT_GE(*res.greeks.gamma_std_error, 0.0);
        EXPECT_GE(*res.greeks.theta_std_error, 0.0);
        EXPECT_GE(*res.greeks.rho_std_error, 0.0);
    }

    // ---- Monotonicity in volatility ----------------------------------------

    // Higher vol -> higher lookback price (true for both fixed and floating).
    TEST(LookbackMC, PriceIncreasesWithVol)
    {
        auto price_with_vol = [](Real v)
        {
            LookbackBSInput in{};
            in.spot = S0;
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.dividend = q;
            in.vol = v;
            in.is_call = true;
            in.style = LookbackStyle::FixedStrike;
            in.extremum = LookbackExtremum::Maximum;
            in.n_paths = 200000;
            in.seed = 99;
            PricingRequest request{
                InstrumentKind::EquityLookbackOption,
                ModelKind::BlackScholes,
                EngineKind::MonteCarlo,
                PricingInput{in}};
            return default_registry().price(request).npv;
        };

        EXPECT_LT(price_with_vol(0.10), price_with_vol(0.30));
        EXPECT_LT(price_with_vol(0.30), price_with_vol(0.50));
    }

    // ---- Reproducibility ---------------------------------------------------

    TEST(LookbackMC, ReproducibleWithSeed)
    {
        const PricingResult p1 = price_lookback(
            true, LookbackStyle::FixedStrike, LookbackExtremum::Maximum, 100000, 11);
        const PricingResult p2 = price_lookback(
            true, LookbackStyle::FixedStrike, LookbackExtremum::Maximum, 100000, 11);

        EXPECT_NEAR(p1.npv, p2.npv, 1e-12);
        EXPECT_NEAR(p1.mc_std_error, p2.mc_std_error, 1e-12);
    }

    // ---- Validation --------------------------------------------------------

    TEST(LookbackMC, AmericanExerciseThrows)
    {
        using namespace quantModeling;

        auto payoff = std::make_shared<PlainVanillaPayoff>(OptionType::Call, K);
        auto exercise = std::make_shared<AmericanExercise>(T);
        LookbackOption opt(payoff, exercise, LookbackStyle::FixedStrike, LookbackExtremum::Maximum, 1.0);

        auto model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);
        PricingSettings settings;
        settings.mc_paths = 1000;
        settings.mc_seed = 1;
        MarketView market = {};
        PricingContext ctx{market, settings, model};
        BSEuroLookbackMCEngine engine(ctx);

        EXPECT_THROW(opt.accept(engine), UnsupportedInstrument);
    }

} // namespace quantModeling
