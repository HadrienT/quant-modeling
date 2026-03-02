#ifndef PRICERS_ADAPTERS_EQUITY_AUTOCALL_HPP
#define PRICERS_ADAPTERS_EQUITY_AUTOCALL_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_autocall_bs_mc(const AutocallBSInput &in);

} // namespace quantModeling

#endif
