#ifndef PRICERS_ADAPTERS_EQUITY_BARRIER_LV_HPP
#define PRICERS_ADAPTERS_EQUITY_BARRIER_LV_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    PricingResult price_equity_barrier_lv_mc(const BarrierLocalVolInput &in);
} // namespace quantModeling

#endif
