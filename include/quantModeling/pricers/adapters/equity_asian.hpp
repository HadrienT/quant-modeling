#ifndef PRICERS_ADAPTERS_EQUITY_ASIAN_HPP
#define PRICERS_ADAPTERS_EQUITY_ASIAN_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_asian_bs(const AsianBSInput &in, EngineKind engine);

} // namespace quantModeling

#endif
