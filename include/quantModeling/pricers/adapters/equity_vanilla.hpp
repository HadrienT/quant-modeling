#ifndef PRICERS_ADAPTERS_EQUITY_VANILLA_HPP
#define PRICERS_ADAPTERS_EQUITY_VANILLA_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_vanilla_bs(const VanillaBSInput &in, EngineKind engine);

} // namespace quantModeling

#endif
