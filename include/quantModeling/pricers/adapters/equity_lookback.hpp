#ifndef PRICERS_ADAPTERS_EQUITY_LOOKBACK_HPP
#define PRICERS_ADAPTERS_EQUITY_LOOKBACK_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_lookback_bs_mc(const LookbackBSInput &in);

} // namespace quantModeling

#endif
