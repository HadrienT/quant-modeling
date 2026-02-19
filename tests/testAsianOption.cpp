#include <gtest/gtest.h>
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/engines/analytic/asian.hpp"
#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/registry.hpp"
#include <memory>
#include <cmath>

namespace quantModeling
{

    // ============================================================================
    // Test Fixtures
    // ============================================================================

    class AsianOptionTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Standard test parameters
            S0 = 100.0;
            K = 100.0;
            T = 1.0;
            r = 0.05;
            q = 0.02;
            sigma = 0.20;

            // Create Black-Scholes model with parameters
            model = std::make_shared<BlackScholesModel>(S0, r, q, sigma);

            // Create pricing context with market view, settings, and model
            market = {};
            settings.mc_paths = 10000;
            settings.mc_seed = 42;
            settings.mc_antithetic = false;

            ctx.market = market;
            ctx.settings = settings;
            ctx.model = model;
        }

        Real S0, K, T, r, q, sigma;
        std::shared_ptr<const ILocalVolModel> model;
        MarketView market;
        PricingSettings settings;
        PricingContext ctx;

        PricingResult price_asian_registry(bool is_call, AsianAverageType average_type, EngineKind engine_kind)
        {
            AsianBSInput in{S0, K, T, r, q, sigma, is_call, average_type};
            if (engine_kind == EngineKind::MonteCarlo)
            {
                in.n_paths = settings.mc_paths;
                in.seed = settings.mc_seed;
            }

            PricingRequest request{
                InstrumentKind::EquityAsianOption,
                ModelKind::BlackScholes,
                engine_kind,
                PricingInput{in}};

            return default_registry().price(request);
        }
    };

    // ============================================================================
    // Payoff Tests
    // ============================================================================

    TEST_F(AsianOptionTest, ArithmeticAsianPayoffCall)
    {
        ArithmeticAsianPayoff payoff(OptionType::Call, K);

        // Test ITM: average spot > strike
        EXPECT_NEAR(payoff(110.0), 10.0, 1e-10);

        // Test ATM: average spot = strike
        EXPECT_NEAR(payoff(K), 0.0, 1e-10);

        // Test OTM: average spot < strike
        EXPECT_NEAR(payoff(90.0), 0.0, 1e-10);

        // Test far OTM
        EXPECT_NEAR(payoff(50.0), 0.0, 1e-10);
    }

    TEST_F(AsianOptionTest, ArithmeticAsianPayoffPut)
    {
        ArithmeticAsianPayoff payoff(OptionType::Put, K);

        // Test ITM: average spot < strike
        EXPECT_NEAR(payoff(90.0), 10.0, 1e-10);

        // Test ATM: average spot = strike
        EXPECT_NEAR(payoff(K), 0.0, 1e-10);

        // Test OTM: average spot > strike
        EXPECT_NEAR(payoff(110.0), 0.0, 1e-10);
    }

    TEST_F(AsianOptionTest, GeometricAsianPayoffCall)
    {
        GeometricAsianPayoff payoff(OptionType::Call, K);

        // Test ITM
        EXPECT_NEAR(payoff(120.0), 20.0, 1e-10);

        // Test ATM
        EXPECT_NEAR(payoff(K), 0.0, 1e-10);

        // Test OTM
        EXPECT_NEAR(payoff(80.0), 0.0, 1e-10);
    }

    TEST_F(AsianOptionTest, GeometricAsianPayoffPut)
    {
        GeometricAsianPayoff payoff(OptionType::Put, K);

        // Test ITM
        EXPECT_NEAR(payoff(80.0), 20.0, 1e-10);

        // Test ATM
        EXPECT_NEAR(payoff(K), 0.0, 1e-10);

        // Test OTM
        EXPECT_NEAR(payoff(120.0), 0.0, 1e-10);
    }

    // ============================================================================
    // Analytic Engine Tests
    // ============================================================================

    TEST_F(AsianOptionTest, ArithmeticAsianAnalyticCall)
    {
        const PricingResult result = price_asian_registry(true, AsianAverageType::Arithmetic, EngineKind::Analytic);

        // Price should be positive
        EXPECT_GT(result.npv, 0.0);

        // Price should be less than intrinsic value plus time value bound
        EXPECT_LT(result.npv, S0);

        // Delta should be between 0 and 1 for ATM call
        EXPECT_GT(result.greeks.delta, 0.0);
        EXPECT_LT(result.greeks.delta, 1.0);

        // Gamma should be positive
        EXPECT_GT(result.greeks.gamma, 0.0);

        // Vega should be positive
        EXPECT_GT(result.greeks.vega, 0.0);
    }

    TEST_F(AsianOptionTest, ArithmeticAsianAnalyticPut)
    {
        const PricingResult result = price_asian_registry(false, AsianAverageType::Arithmetic, EngineKind::Analytic);

        // Price should be positive
        EXPECT_GT(result.npv, 0.0);

        // Delta should be between -1 and 0 for ATM put
        EXPECT_LT(result.greeks.delta, 0.0);
        EXPECT_GT(result.greeks.delta, -1.0);
    }

    TEST_F(AsianOptionTest, GeometricAsianAnalyticCall)
    {
        const PricingResult result = price_asian_registry(true, AsianAverageType::Geometric, EngineKind::Analytic);

        // Price should be positive and less than arithmetic
        EXPECT_GT(result.npv, 0.0);
        EXPECT_LT(result.npv, S0);
    }

    // ============================================================================
    // Monte Carlo Engine Tests
    // ============================================================================

    TEST_F(AsianOptionTest, AsianMCArithmeticCall)
    {
        ctx.settings.mc_paths = 50000;
        settings.mc_paths = ctx.settings.mc_paths;

        const PricingResult result = price_asian_registry(true, AsianAverageType::Arithmetic, EngineKind::MonteCarlo);

        // Price should be positive
        EXPECT_GT(result.npv, 0.0);

        // Standard error should be small relative to price
        EXPECT_GT(result.mc_std_error, 0.0);
        EXPECT_LT(result.mc_std_error, result.npv * 0.1); // Less than 10% of price
    }

    TEST_F(AsianOptionTest, AsianMCGeometricCall)
    {
        ctx.settings.mc_paths = 50000;
        settings.mc_paths = ctx.settings.mc_paths;

        const PricingResult result = price_asian_registry(true, AsianAverageType::Geometric, EngineKind::MonteCarlo);

        // Price should be positive
        EXPECT_GT(result.npv, 0.0);

        // Standard error should be reasonable
        EXPECT_GT(result.mc_std_error, 0.0);
    }

    // ============================================================================
    // Validation Tests
    // ============================================================================

    TEST_F(AsianOptionTest, NullPayoffThrows)
    {
        auto exercise = std::make_shared<EuropeanExercise>(T);
        AsianOption option(nullptr, exercise, AsianAverageType::Arithmetic, 1.0);

        BSEuroArithmeticAsianAnalyticEngine engine(ctx);

        EXPECT_THROW(
            option.accept(engine),
            InvalidInput);
    }

    TEST_F(AsianOptionTest, NullExerciseThrows)
    {
        auto payoff = std::make_shared<ArithmeticAsianPayoff>(OptionType::Call, K);
        AsianOption option(payoff, nullptr, AsianAverageType::Arithmetic, 1.0);

        BSEuroArithmeticAsianAnalyticEngine engine(ctx);

        EXPECT_THROW(
            option.accept(engine),
            InvalidInput);
    }

    // ============================================================================
    // Comparison Tests
    // ============================================================================

    TEST_F(AsianOptionTest, AnalyticVsMCConvergence)
    {
        const PricingResult analyticResult =
            price_asian_registry(true, AsianAverageType::Arithmetic, EngineKind::Analytic);
        Real analyticPrice = analyticResult.npv;

        // Get MC price with high paths
        ctx.settings.mc_paths = 500000;
        settings.mc_paths = ctx.settings.mc_paths;
        const PricingResult mcResult =
            price_asian_registry(true, AsianAverageType::Arithmetic, EngineKind::MonteCarlo);
        Real mcPrice = mcResult.npv;
        Real mcStdErr = mcResult.mc_std_error;

        // MC should converge to analytic within 2 standard errors
        EXPECT_LT(std::abs(mcPrice - analyticPrice), 2.0 * mcStdErr);
    }

    TEST_F(AsianOptionTest, ArithmeticGeometricOrdering)
    {
        // Geometric average <= Arithmetic average (Jensen's inequality)
        // So geometric option <= arithmetic option

        const PricingResult arithResult =
            price_asian_registry(true, AsianAverageType::Arithmetic, EngineKind::Analytic);
        const PricingResult geomResult =
            price_asian_registry(true, AsianAverageType::Geometric, EngineKind::Analytic);

        Real arithPrice = arithResult.npv;
        Real geomPrice = geomResult.npv;

        // Geometric price should be strictly less than arithmetic for ATM options
        EXPECT_LT(geomPrice, arithPrice);
        EXPECT_GT(arithPrice - geomPrice, 0.01); // Meaningful difference
    }

} // namespace quantModeling

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
