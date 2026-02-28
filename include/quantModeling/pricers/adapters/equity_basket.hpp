#ifndef PRICERS_ADAPTERS_EQUITY_BASKET_HPP
#define PRICERS_ADAPTERS_EQUITY_BASKET_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_basket_bs_mc(const BasketBSInput &in);

} // namespace quantModeling

#endif
