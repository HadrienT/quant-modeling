#ifndef PRICERS_ADAPTERS_EQUITY_ASIAN_LV_HPP
#define PRICERS_ADAPTERS_EQUITY_ASIAN_LV_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    PricingResult price_equity_asian_lv_mc(const AsianLocalVolInput &in);
} // namespace quantModeling

#endif
