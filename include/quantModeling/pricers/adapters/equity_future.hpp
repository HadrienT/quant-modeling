#ifndef PRICERS_ADAPTERS_EQUITY_FUTURE_HPP
#define PRICERS_ADAPTERS_EQUITY_FUTURE_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_future_bs(const EquityFutureInput &in);

} // namespace quantModeling

#endif
