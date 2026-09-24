#ifndef PRICERS_ADAPTERS_EQUITY_QUANTO_HPP
#define PRICERS_ADAPTERS_EQUITY_QUANTO_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    /// European quanto option (models/equity/quanto_black_scholes.hpp), on the
    /// vanilla Black-Scholes engines: Analytic or MonteCarlo.
    PricingResult price_quanto_vanilla_bs(const QuantoBSInput &in, EngineKind engine);

} // namespace quantModeling

#endif
