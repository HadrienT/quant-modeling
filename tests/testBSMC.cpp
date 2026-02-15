#include <gtest/gtest.h>

#include "quantModeling/instruments/base.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/registry.hpp"

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

    PricingResult priceEuroVanillaMC(OptionType optType, PricingSettings settings)
    {
      VanillaBSInput in{S0, K, T, r, q, sigma, optType == OptionType::Call};
      in.n_paths = settings.mc_paths;
      in.seed = settings.mc_seed;
      in.mc_epsilon = 0.0;

      PricingRequest request{
          InstrumentKind::EquityVanillaOption,
          ModelKind::BlackScholes,
          EngineKind::MonteCarlo,
          PricingInput{in}};
      return default_registry().price(request);
    }

    PricingResult priceEuroVanillaAnalytical(OptionType optType)
    {
      VanillaBSInput in{S0, K, T, r, q, sigma, optType == OptionType::Call};
      PricingRequest request{
          InstrumentKind::EquityVanillaOption,
          ModelKind::BlackScholes,
          EngineKind::Analytic,
          PricingInput{in}};
      return default_registry().price(request);
    }
  } // namespace

  TEST(BSMC, ReproducibleWithFixedSeed)
  {
    PricingSettings s1 = {1'000'000, 1, 0};
    PricingResult res1 = priceEuroVanillaMC(OptionType::Call, s1);
    PricingResult res2 = priceEuroVanillaMC(OptionType::Call, s1);

    EXPECT_NEAR(res1.npv, res2.npv, 1e-12);

    res1 = priceEuroVanillaMC(OptionType::Put, s1);
    res2 = priceEuroVanillaMC(OptionType::Put, s1);

    EXPECT_NEAR(res1.npv, res2.npv, 1e-12);
  }

  TEST(BSMC, PriceMatchesAnalyticWithin3Sigma)
  {
    PricingSettings s1 = {1'000'000, 1, 0};

    const PricingResult mc = priceEuroVanillaMC(OptionType::Call, s1);
    const PricingResult ana = priceEuroVanillaAnalytical(OptionType::Call);
    Real err = mc.mc_std_error;
    EXPECT_LE(std::abs(mc.npv - ana.npv), 3.0 * err);
  }

  TEST(BSMC, ErrorDecreasesWhenIncreasingPaths)
  {
    const auto ana = priceEuroVanillaAnalytical(OptionType::Call).npv;

    PricingSettings s1{};
    s1.mc_seed = 1;
    s1.mc_paths = 100'000;

    PricingSettings s2 = s1;
    s2.mc_paths = 400'000;

    const Real e1 = std::abs(priceEuroVanillaMC(OptionType::Call, s1).npv - ana);
    const Real e2 = std::abs(priceEuroVanillaMC(OptionType::Call, s2).npv - ana);

    EXPECT_LT(e2, e1);
    EXPECT_LT(
        e2,
        0.5 * e1); // MC converges in 1 / sqrt(N), 0.5 works only with antithetic;
  }

  TEST(BSMC, CallPutParity)
  {
    PricingSettings s{};
    s.mc_seed = 100;
    s.mc_paths = 2'000'000;

    const Real C = priceEuroVanillaMC(OptionType::Call, s).npv;
    const Real P = priceEuroVanillaMC(OptionType::Put, s).npv;

    const Real rhs = S0 * std::exp(-q * T) - K * std::exp(-r * T);
    EXPECT_NEAR(C - P, rhs, 1e-2);
  }

} // namespace quantModeling