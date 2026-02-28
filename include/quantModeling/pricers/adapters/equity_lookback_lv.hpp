#ifndef PRICERS_ADAPTERS_EQUITY_LOOKBACK_LV_HPP
#define PRICERS_ADAPTERS_EQUITY_LOOKBACK_LV_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    PricingResult price_equity_lookback_lv_mc(const LookbackLocalVolInput &in);
} // namespace quantModeling

#endif
