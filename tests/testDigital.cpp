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

        // ── Helpers ─────────────────────────────────────────────────────────
        PricingResult priceDigital(bool is_call,
                                   DigitalPayoffType payoff_type,
                                   Real cash_amount = 1.0,
                                   Real strike = K,
                                   Real maturity = T)
        {
            DigitalBSInput in{};
            in.spot = S0;
            in.strike = strike;
            in.maturity = maturity;
            in.rate = r;
            in.dividend = q;
            in.vol = sigma;
            in.is_call = is_call;
            in.payoff_type = payoff_type;
            in.cash_amount = cash_amount;

            PricingRequest request{
                InstrumentKind::EquityDigitalOption,
                ModelKind::BlackScholes,
                EngineKind::Analytic,
                PricingInput{in}};
            return default_registry().price(request);
        }

        // ── Closed-form reference values (BS digital) ───────────────────────
        // d2 = [ln(S/K) + (r - q - 0.5*sigma^2)*T] / (sigma*sqrt(T))
        Real bs_d2()
        {
            return (std::log(S0 / K) + (r - q - 0.5 * sigma * sigma) * T) /
                   (sigma * std::sqrt(T));
        }
        Real bs_d1()
        {
            return bs_d2() + sigma * std::sqrt(T);
        }

    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Cash-or-nothing
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DigitalAnalytic, CashOrNothingCallPrice)
    {
        const auto res = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0);
        // Reference: e^{-rT} * N(d2)
        const Real expected = std::exp(-r * T) * 0.5 * std::erfc(-bs_d2() / std::sqrt(2.0));
        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(DigitalAnalytic, CashOrNothingPutPrice)
    {
        const auto res = priceDigital(false, DigitalPayoffType::CashOrNothing, 1.0);
        // Reference: e^{-rT} * N(-d2)
        const Real expected = std::exp(-r * T) * 0.5 * std::erfc(bs_d2() / std::sqrt(2.0));
        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(DigitalAnalytic, CashOrNothingCallPutParity)
    {
        // N(d2) + N(-d2) = 1  →  call + put = e^{-rT} * cash
        const Real C = priceDigital(true, DigitalPayoffType::CashOrNothing, 5.0).npv;
        const Real P = priceDigital(false, DigitalPayoffType::CashOrNothing, 5.0).npv;
        const Real expected = 5.0 * std::exp(-r * T);
        EXPECT_NEAR(C + P, expected, 1e-10);
    }

    TEST(DigitalAnalytic, CashOrNothingScalesWithCash)
    {
        const Real p1 = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0).npv;
        const Real p10 = priceDigital(true, DigitalPayoffType::CashOrNothing, 10.0).npv;
        EXPECT_NEAR(p10, 10.0 * p1, 1e-10);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Asset-or-nothing
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DigitalAnalytic, AssetOrNothingCallPrice)
    {
        const auto res = priceDigital(true, DigitalPayoffType::AssetOrNothing);
        // Reference: S0 * e^{-qT} * N(d1)
        const Real expected = S0 * std::exp(-q * T) * 0.5 * std::erfc(-bs_d1() / std::sqrt(2.0));
        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(DigitalAnalytic, AssetOrNothingPutPrice)
    {
        const auto res = priceDigital(false, DigitalPayoffType::AssetOrNothing);
        // Reference: S0 * e^{-qT} * N(-d1)
        const Real expected = S0 * std::exp(-q * T) * 0.5 * std::erfc(bs_d1() / std::sqrt(2.0));
        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(DigitalAnalytic, AssetOrNothingCallPutParity)
    {
        // N(d1) + N(-d1) = 1  →  call + put = S0 * e^{-qT}
        const Real C = priceDigital(true, DigitalPayoffType::AssetOrNothing).npv;
        const Real P = priceDigital(false, DigitalPayoffType::AssetOrNothing).npv;
        const Real expected = S0 * std::exp(-q * T);
        EXPECT_NEAR(C + P, expected, 1e-10);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Vanilla = Asset-or-nothing call - Cash-or-nothing call (with cash = K)
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DigitalAnalytic, VanillaDecomposition)
    {
        // Vanilla call = AoN call - K * CoN call
        const Real aon = priceDigital(true, DigitalPayoffType::AssetOrNothing).npv;
        const Real con = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0).npv;
        const Real digital_vanilla = aon - K * con;

        // Compare with BS analytic vanilla
        VanillaBSInput van{S0, K, T, r, q, sigma, true};
        PricingRequest req{InstrumentKind::EquityVanillaOption,
                           ModelKind::BlackScholes, EngineKind::Analytic,
                           PricingInput{van}};
        const Real vanilla_price = default_registry().price(req).npv;

        EXPECT_NEAR(digital_vanilla, vanilla_price, 1e-10);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Monotonicity / boundary tests
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DigitalAnalytic, CashCallMonotoneInSpot)
    {
        // Deep ITM digital call (low K) → N(d2) → 1
        const Real deep_itm = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0, 50.0).npv;
        const Real atm = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0, 100.0).npv;
        const Real deep_otm = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0, 150.0).npv;

        EXPECT_GT(deep_itm, atm);
        EXPECT_GT(atm, deep_otm);
    }

    TEST(DigitalAnalytic, DeepITMCashCallApproachesDiscount)
    {
        // Very deep ITM: K = 1, S0 = 100 → N(d2) ≈ 1
        const Real npv = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0, 1.0).npv;
        const Real df = std::exp(-r * T);
        EXPECT_NEAR(npv, df, 1e-3);
    }

    TEST(DigitalAnalytic, DeepOTMCashCallNearZero)
    {
        // Very deep OTM: K = 1000, S0 = 100 → N(d2) ≈ 0
        const Real npv = priceDigital(true, DigitalPayoffType::CashOrNothing, 1.0, 1000.0).npv;
        EXPECT_NEAR(npv, 0.0, 1e-6);
    }

    TEST(DigitalAnalytic, PositivePrice)
    {
        EXPECT_GT(priceDigital(true, DigitalPayoffType::CashOrNothing).npv, 0.0);
        EXPECT_GT(priceDigital(false, DigitalPayoffType::CashOrNothing).npv, 0.0);
        EXPECT_GT(priceDigital(true, DigitalPayoffType::AssetOrNothing).npv, 0.0);
        EXPECT_GT(priceDigital(false, DigitalPayoffType::AssetOrNothing).npv, 0.0);
    }

    TEST(DigitalAnalytic, DiagnosticsContainType)
    {
        auto res = priceDigital(true, DigitalPayoffType::CashOrNothing);
        EXPECT_NE(res.diagnostics.find("cash-or-nothing"), std::string::npos);

        res = priceDigital(true, DigitalPayoffType::AssetOrNothing);
        EXPECT_NE(res.diagnostics.find("asset-or-nothing"), std::string::npos);
    }

} // namespace quantModeling
