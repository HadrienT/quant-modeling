#include <gtest/gtest.h>

#include "quantModeling/engines/mc/basket.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    namespace
    {

        // ── Test parameters ──────────────────────────────────────────────────────────
        constexpr Real r = 0.05;
        constexpr Real q1 = 0.02;
        constexpr Real q2 = 0.02;
        constexpr Real sig1 = 0.20;
        constexpr Real sig2 = 0.25;
        constexpr Real S1 = 100.0;
        constexpr Real S2 = 100.0;
        constexpr Real K = 100.0;
        constexpr Real T = 1.0;

        // ── Helpers ───────────────────────────────────────────────────────────────────
        PricingResult price_vanilla_analytic(bool is_call)
        {
            VanillaBSInput in{S1, K, T, r, q1, sig1, is_call};
            PricingRequest request{
                InstrumentKind::EquityVanillaOption,
                ModelKind::BlackScholes, EngineKind::Analytic,
                PricingInput{in}};
            return default_registry().price(request);
        }

        PricingResult price_basket(bool is_call, double rho,
                                   int n_paths = 200000, int seed = 42)
        {
            BasketBSInput in{};
            in.spots = {S1, S2};
            in.vols = {sig1, sig2};
            in.dividends = {q1, q2};
            in.weights = {0.5, 0.5};
            in.correlations = {{1.0, rho}, {rho, 1.0}};
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.is_call = is_call;
            in.n_paths = n_paths;
            in.seed = seed;
            in.mc_antithetic = true;

            PricingRequest request{
                InstrumentKind::EquityBasketOption,
                ModelKind::BlackScholes, EngineKind::MonteCarlo,
                PricingInput{in}};
            return default_registry().price(request);
        }

        // ── Tests ─────────────────────────────────────────────────────────────────────

        // 1. Basket price is strictly positive for ATM call.
        TEST(BasketMC, CallIsPositive)
        {
            const auto res = price_basket(true, 0.5);
            EXPECT_GT(res.npv, 0.0);
        }

        // 2. Basket put price is strictly positive for ATM put.
        TEST(BasketMC, PutIsPositive)
        {
            const auto res = price_basket(false, 0.5);
            EXPECT_GT(res.npv, 0.0);
        }

        // 3. Call price increases with correlation (higher rho → higher basket vol → higher call price).
        TEST(BasketMC, CallPriceIncreasesWithCorrelation)
        {
            const auto lo = price_basket(true, -0.5);
            const auto mid = price_basket(true, 0.0);
            const auto hi = price_basket(true, 0.9);
            EXPECT_LT(lo.npv, mid.npv) << "low rho should give cheaper basket call";
            EXPECT_LT(mid.npv, hi.npv) << "high rho should give more expensive basket call";
        }

        // 4. Put price increases with correlation (symmetric reasoning to call).
        TEST(BasketMC, PutPriceIncreasesWithCorrelation)
        {
            const auto lo = price_basket(false, -0.5);
            const auto hi = price_basket(false, 0.9);
            EXPECT_LT(lo.npv, hi.npv) << "high rho basket put should be more expensive";
        }

        // 5. With perfectly correlated equal-volatility assets (rho=1), basket call ≈ vanilla call.
        //    Basket(S1,S2,rho=1,w=0.5,0.5) with sig1=sig2=sigma and S1=S2 is identical to vanilla.
        TEST(BasketMC, PerfectlyCorrelatedMatchesVanilla)
        {
            // Use identical assets: sig1=sig2, S1=S2 → basket is single asset priced analytically.
            const double sigma = 0.20;
            BasketBSInput in{};
            in.spots = {100.0, 100.0};
            in.vols = {sigma, sigma};
            in.dividends = {q1, q1};
            in.weights = {0.5, 0.5};
            in.correlations = {{1.0, 1.0}, {1.0, 1.0}}; // NOTE: singular, but valid for this test
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.is_call = true;
            in.n_paths = 300000;
            in.seed = 1;
            in.mc_antithetic = true;

            PricingRequest req{
                InstrumentKind::EquityBasketOption,
                ModelKind::BlackScholes, EngineKind::MonteCarlo,
                PricingInput{in}};
            const auto basket_res = default_registry().price(req);
            const auto vanilla_res = price_vanilla_analytic(true);

            // MC noise: allow 3% relative tolerance
            EXPECT_NEAR(basket_res.npv, vanilla_res.npv, vanilla_res.npv * 0.03)
                << "Perfectly correlated equal-asset basket should match vanilla call";
        }

        // 6. With zero correlation, basket call is cheaper than vanilla call
        //    (averaging reduces effective vol).
        TEST(BasketMC, ZeroCorrelationCheaperThanVanilla)
        {
            const auto basket_call = price_basket(true, 0.0);
            const auto vanilla_call = price_vanilla_analytic(true);
            // Basket vol_eff = sqrt(w1^2*sig1^2 + w2^2*sig2^2) < max(sig1,sig2)
            EXPECT_LT(basket_call.npv, vanilla_call.npv)
                << "Zero-correlation basket call should be cheaper than vanilla on sig1";
        }

        // 7. Greeks are populated and pass sign/magnitude sanity checks.
        TEST(BasketMC, GreeksPopulated)
        {
            const auto res = price_basket(true, 0.5, 200000, 99);
            ASSERT_TRUE(res.greeks.delta.has_value());
            ASSERT_TRUE(res.greeks.gamma.has_value());
            ASSERT_TRUE(res.greeks.vega.has_value());
            ASSERT_TRUE(res.greeks.rho.has_value());
            ASSERT_TRUE(res.greeks.theta.has_value());

            EXPECT_GT(*res.greeks.delta, 0.0) << "Call delta > 0";
            EXPECT_GT(*res.greeks.gamma, 0.0) << "Gamma > 0";
            EXPECT_GT(*res.greeks.vega, 0.0) << "Vega > 0";
            EXPECT_GT(*res.greeks.rho, 0.0) << "Call rho > 0";
            EXPECT_LT(*res.greeks.theta, 0.0) << "Theta < 0 (time decay)";
        }

        // 8. Empty correlations matrix defaults to identity (zero pairwise correlation).
        TEST(BasketMC, EmptyCorrelationsDefaultsToIdentity)
        {
            BasketBSInput in_empty{};
            in_empty.spots = {S1, S2};
            in_empty.vols = {sig1, sig2};
            in_empty.dividends = {q1, q2};
            in_empty.weights = {0.5, 0.5};
            // correlations left empty → identity
            in_empty.strike = K;
            in_empty.maturity = T;
            in_empty.rate = r;
            in_empty.is_call = true;
            in_empty.n_paths = 200000;
            in_empty.seed = 7;
            in_empty.mc_antithetic = true;

            const auto zero_rho = price_basket(true, 0.0, 200000, 7); // explicit rho=0

            PricingRequest req{
                InstrumentKind::EquityBasketOption,
                ModelKind::BlackScholes, EngineKind::MonteCarlo,
                PricingInput{in_empty}};
            const auto identity_res = default_registry().price(req);

            EXPECT_NEAR(zero_rho.npv, identity_res.npv, zero_rho.npv * 0.005)
                << "Empty correlations should match explicit rho=0";
        }

        // 9. Reproducibility: same seed → same price.
        TEST(BasketMC, Reproducibility)
        {
            const auto r1 = price_basket(true, 0.3, 100000, 123);
            const auto r2 = price_basket(true, 0.3, 100000, 123);
            EXPECT_DOUBLE_EQ(r1.npv, r2.npv);
        }

        // 10. Invalid input: wrong correlation matrix size throws InvalidInput.
        TEST(BasketMC, WrongCorrelationSizeThrows)
        {
            BasketBSInput in{};
            in.spots = {S1, S2};
            in.vols = {sig1, sig2};
            in.dividends = {q1, q2};
            in.weights = {0.5, 0.5};
            in.correlations = {{1.0, 0.3}}; // 1×2 instead of 2×2 → invalid
            in.strike = K;
            in.maturity = T;
            in.rate = r;
            in.is_call = true;

            PricingRequest req{
                InstrumentKind::EquityBasketOption,
                ModelKind::BlackScholes, EngineKind::MonteCarlo,
                PricingInput{in}};
            EXPECT_THROW(default_registry().price(req), InvalidInput);
        }

    } // namespace
} // namespace quantModeling
