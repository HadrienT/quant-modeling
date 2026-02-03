#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Dense>
#include <iostream>
#include <memory>

int main() {
  using namespace quantModeling;

  // Example: Call option, S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%,
  // notional=1
  auto payoff = std::make_shared<PlainVanillaPayoff>(OptionType::Put, 100.0);
  auto exercise = std::make_shared<EuropeanExercise>(1.0);
  VanillaOption opt(payoff, exercise, 1.0);

  auto model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
  MarketView market = {};
  PricingSettings settings = {1'000'000, 1, 0.0};
  PricingContext ctx{market, settings, model};

  BSEuroVanillaAnalyticEngine engine(ctx);
  BSEuroVanillaMCEngine engineMC(ctx);

  PricingResult res = price(opt, engine);

  std::cout << "NPV: " << res.npv << "\n";
  std::cout << "Delta: " << (res.greeks.delta ? *res.greeks.delta : 0.0)
            << "\n";
  std::cout << "Gamma: " << (res.greeks.gamma ? *res.greeks.gamma : 0.0)
            << "\n";
  std::cout << "Vega:  " << (res.greeks.vega ? *res.greeks.vega : 0.0) << "\n";
  std::cout << "Theta: " << (res.greeks.theta ? *res.greeks.theta : 0.0)
            << "\n";
  std::cout << "Rho:   " << (res.greeks.rho ? *res.greeks.rho : 0.0) << "\n";
  std::cout << "Diag:  " << res.diagnostics << "\n";

  std::cout << "\n";
  res = price(opt, engineMC);
  std::cout << "NPV: " << res.npv << "\n";
  std::cout << "Delta: " << (res.greeks.delta ? *res.greeks.delta : 0.0)
            << "\n";
  std::cout << "Gamma: " << (res.greeks.gamma ? *res.greeks.gamma : 0.0)
            << "\n";
  std::cout << "Vega:  " << (res.greeks.vega ? *res.greeks.vega : 0.0) << "\n";
  std::cout << "Theta: " << (res.greeks.theta ? *res.greeks.theta : 0.0)
            << "\n";
  std::cout << "Rho:   " << (res.greeks.rho ? *res.greeks.rho : 0.0) << "\n";
  std::cout << "Diag:  " << res.diagnostics << "\n";

  return 0;
}
