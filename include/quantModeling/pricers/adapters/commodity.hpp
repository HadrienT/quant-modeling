#ifndef ADAPTERS_COMMODITY_HPP
#define ADAPTERS_COMMODITY_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_commodity_forward_analytic(const CommodityForwardInput &in);
    PricingResult price_commodity_option_analytic(const CommodityOptionInput &in);

} // namespace quantModeling

#endif
