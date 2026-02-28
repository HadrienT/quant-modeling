#ifndef PRICERS_ADAPTERS_EQUITY_BARRIER_HPP
#define PRICERS_ADAPTERS_EQUITY_BARRIER_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    PricingResult price_equity_barrier_bs_mc(const BarrierBSInput &in);

} // namespace quantModeling

#endif // PRICERS_ADAPTERS_EQUITY_BARRIER_HPP
