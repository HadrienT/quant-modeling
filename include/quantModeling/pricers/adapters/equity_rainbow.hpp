#ifndef PRICERS_ADAPTERS_EQUITY_RAINBOW_HPP
#define PRICERS_ADAPTERS_EQUITY_RAINBOW_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_worst_of_bs_mc(const RainbowBSInput &in);
    PricingResult price_best_of_bs_mc(const RainbowBSInput &in);

} // namespace quantModeling

#endif
