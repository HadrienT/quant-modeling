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
    };

    struct PricingResult
    {
        Real npv = 0.0;
        Greeks greeks;
        std::string diagnostics;
    };
}

#endif
