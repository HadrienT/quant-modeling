#include <gtest/gtest.h>

#include "quantModeling/instruments/base.hpp"
#include "quantModeling/pricers/registry.hpp"

#include <cmath>
#include <memory>
#include <string>

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
    constexpr Real N = 1.0;

    constexpr Real RefCall = 9.22701;
    constexpr Real RefPut = 6.33008;

    constexpr Real RefCallDelta = 0.586851;
    constexpr Real RefCallGamma = 0.0189506;
    constexpr Real RefCallVega = 37.9012;
    constexpr Real RefCallTheta = -5.08932;
    constexpr Real RefCallRho = 49.4581;

    constexpr Real RefPutDelta = -0.393348;
    constexpr Real RefPutGamma = 0.0189506;
    constexpr Real RefPutVega = 37.9012;
    constexpr Real RefPutTheta = -2.29357;
    constexpr Real RefPutRho = -45.6648;

    inline PricingResult priceEuroVanilla(OptionType type, Real strike = K,
                                          Real maturity = T, Real notional = N)
    {
      (void)notional;
      VanillaBSInput in{S0, strike, maturity, r, q, sigma, type == OptionType::Call};
      PricingRequest request{
          InstrumentKind::EquityVanillaOption,
          ModelKind::BlackScholes,
          EngineKind::Analytic,
          PricingInput{in}};
      return default_registry().price(request);
    }

    inline Real requireGreek(const std::optional<Real> &g)
    {
      EXPECT_TRUE(g.has_value());
      return g.value_or(0.0);
    }

  } // namespace

  class BSAnalyticFixture : public ::testing::Test
  {
  protected:
    static PricingResult call() { return priceEuroVanilla(OptionType::Call); }
    static PricingResult put() { return priceEuroVanilla(OptionType::Put); }
  };

  TEST_F(BSAnalyticFixture, EuroCallPrice)
  {
    const auto res = call();

    EXPECT_NEAR(res.npv, RefCall, 1e-5);

    EXPECT_NEAR(requireGreek(res.greeks.delta), RefCallDelta, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.gamma), RefCallGamma, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.rho), RefCallRho, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.theta), RefCallTheta, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.vega), RefCallVega, 1e-4);
  }

  TEST_F(BSAnalyticFixture, EuroPutPrice)
  {
    const auto res = put();

    EXPECT_NEAR(res.npv, RefPut, 1e-5);

    EXPECT_NEAR(requireGreek(res.greeks.delta), RefPutDelta, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.gamma), RefPutGamma, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.rho), RefPutRho, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.theta), RefPutTheta, 1e-4);
    EXPECT_NEAR(requireGreek(res.greeks.vega), RefPutVega, 1e-4);
  }

  TEST_F(BSAnalyticFixture, CallPutParity)
  {
    const Real C = call().npv;
    const Real P = put().npv;

    // C - P = S0*exp(-qT) - K*exp(-rT)
    const Real lhs = C - P;
    const Real rhs = S0 * std::exp(-q * T) - K * std::exp(-r * T);

    EXPECT_NEAR(lhs, rhs, 1e-10);
  }

  TEST_F(BSAnalyticFixture, ArbitrageBounds)
  {
    const Real discS = S0 * std::exp(-q * T);
    const Real discK = K * std::exp(-r * T);

    const Real C = call().npv;
    const Real P = put().npv;

    EXPECT_GE(C, std::max<Real>(0.0, discS - discK));
    EXPECT_LE(C, discS);

    EXPECT_GE(P, std::max<Real>(0.0, discK - discS));
    EXPECT_LE(P, discK);
  }

  TEST_F(BSAnalyticFixture, GammaSameForCallAndPut)
  {
    const auto rc = call();
    const auto rp = put();

    EXPECT_NEAR(requireGreek(rc.greeks.gamma), requireGreek(rp.greeks.gamma),
                1e-12);
  }

  TEST_F(BSAnalyticFixture, VegaSameForCallAndPut)
  {
    const auto rc = call();
    const auto rp = put();

    EXPECT_NEAR(requireGreek(rc.greeks.vega), requireGreek(rp.greeks.vega),
                1e-12);
  }

  TEST_F(BSAnalyticFixture, DeltaParity)
  {
    const auto rc = call();
    const auto rp = put();

    const Real expected = std::exp(-q * T);
    EXPECT_NEAR((requireGreek(rc.greeks.delta)) - (requireGreek(rp.greeks.delta)),
                expected, 1e-10);
  }

  TEST_F(BSAnalyticFixture, RhoParity)
  {
    const auto rc = call();
    const auto rp = put();

    const Real expected = T * K * std::exp(-r * T);
    EXPECT_NEAR((requireGreek(rc.greeks.rho)) - (requireGreek(rp.greeks.rho)),
                expected, 1e-8);
  }

  TEST(BSAnalytic, CallMonotoneInSpot)
  {
    auto priceCallAt = [](Real spot)
    {
      VanillaBSInput in{spot, K, T, r, q, sigma, true};
      PricingRequest request{
          InstrumentKind::EquityVanillaOption,
          ModelKind::BlackScholes,
          EngineKind::Analytic,
          PricingInput{in}};
      return default_registry().price(request).npv;
    };

    EXPECT_LT(priceCallAt(90.0), priceCallAt(100.0));
    EXPECT_LT(priceCallAt(100.0), priceCallAt(110.0));
  }

  TEST(BSAnalytic, DeltaMatchesFiniteDifference)
  {
    const Real h = 1e-4 * S0;

    auto priceCallSpot = [&](Real spot)
    {
      VanillaBSInput in{spot, K, T, r, q, sigma, true};
      PricingRequest request{
          InstrumentKind::EquityVanillaOption,
          ModelKind::BlackScholes,
          EngineKind::Analytic,
          PricingInput{in}};
      return default_registry().price(request).npv;
    };

    const Real fdDelta =
        (priceCallSpot(S0 + h) - priceCallSpot(S0 - h)) / (2.0 * h);

    const auto res = priceEuroVanilla(OptionType::Call);

    EXPECT_NEAR(requireGreek(res.greeks.delta), fdDelta, 1e-6);
  }

} // namespace quantModeling
