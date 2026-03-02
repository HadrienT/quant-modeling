#ifndef ADAPTERS_EQUITY_DISPERSION_HPP
#define ADAPTERS_EQUITY_DISPERSION_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_dispersion_bs_mc(const DispersionBSInput &in);

} // namespace quantModeling

#endif
