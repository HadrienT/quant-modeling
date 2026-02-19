#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantModeling
{

    TEST(Bonds, ZeroCouponFlatRate)
    {
        ZeroCouponBondInput in{};
        in.maturity = 2.0;
        in.rate = 0.03;
        in.notional = 1000.0;

        PricingRequest request{
            InstrumentKind::ZeroCouponBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};

        const PricingResult res = default_registry().price(request);
        const Real expected = in.notional * std::exp(-in.rate * in.maturity);

        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(Bonds, ZeroCouponCurveLogLinear)
    {
        ZeroCouponBondInput in{};
        in.maturity = 2.0;
        in.rate = 0.02;
        in.notional = 1000.0;
        in.discount_times = {1.0, 3.0};
        in.discount_factors = {0.97, 0.90};

        PricingRequest request{
            InstrumentKind::ZeroCouponBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};

        const PricingResult res = default_registry().price(request);

        const Real log_df = 0.5 * (std::log(0.97) + std::log(0.90));
        const Real df = std::exp(log_df);
        const Real expected = in.notional * df;

        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

    TEST(Bonds, FixedRateCurveSinglePoint)
    {
        FixedRateBondInput in{};
        in.maturity = 1.0;
        in.rate = 0.02;
        in.coupon_rate = 0.05;
        in.coupon_frequency = 1;
        in.notional = 100.0;
        in.discount_times = {1.0};
        in.discount_factors = {0.96};

        PricingRequest request{
            InstrumentKind::FixedRateBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};

        const PricingResult res = default_registry().price(request);

        const Real coupon = in.notional * in.coupon_rate * 1.0;
        const Real expected = (coupon + in.notional) * in.discount_factors.front();

        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

} // namespace quantModeling
