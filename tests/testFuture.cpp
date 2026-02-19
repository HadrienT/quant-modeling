#include <gtest/gtest.h>

#include "quantModeling/pricers/registry.hpp"

#include <cmath>

namespace quantModeling
{

    TEST(EquityFuture, AnalyticMatchesCostOfCarry)
    {
        const Real spot = 100.0;
        const Real strike = 98.0;
        const Real maturity = 1.0;
        const Real rate = 0.05;
        const Real dividend = 0.02;
        const Real notional = 10.0;

        EquityFutureInput in{spot, strike, maturity, rate, dividend, notional};

        PricingRequest request{
            InstrumentKind::EquityFuture,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};

        const PricingResult res = default_registry().price(request);

        const Real forward = spot * std::exp((rate - dividend) * maturity);
        const Real df = std::exp(-rate * maturity);
        const Real expected = notional * (forward - strike) * df;

        EXPECT_NEAR(res.npv, expected, 1e-10);
    }

} // namespace quantModeling
