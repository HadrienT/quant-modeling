#ifndef PRICERS_ADAPTERS_EQUITY_MOUNTAIN_HPP
#define PRICERS_ADAPTERS_EQUITY_MOUNTAIN_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_mountain_bs_mc(const MountainBSInput &in);

} // namespace quantModeling

#endif
