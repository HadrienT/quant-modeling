#ifndef RESULTS_HPP
#define RESULTS_HPP

#include "quantModeling/core/types.hpp"
#include <optional>
#include <string>

namespace quantModeling
{

  struct Greeks
  {
    std::optional<Real> delta;
    std::optional<Real> gamma;
    std::optional<Real> vega;
    std::optional<Real> theta;
    std::optional<Real> rho;
    // Standard errors (Monte Carlo uncertainty) for each Greek
    std::optional<Real> delta_std_error;
    std::optional<Real> gamma_std_error;
    std::optional<Real> vega_std_error;
    std::optional<Real> theta_std_error;
    std::optional<Real> rho_std_error;
  };

  struct PricingResult
  {
    Real npv = 0.0;
    Greeks greeks;
    std::string diagnostics;
    Real mc_std_error;
  };
} // namespace quantModeling

#endif
