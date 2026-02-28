#ifndef PRICERS_ADAPTERS_EQUITY_DIGITAL_HPP
#define PRICERS_ADAPTERS_EQUITY_DIGITAL_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    PricingResult price_equity_digital_bs_analytic(const DigitalBSInput &in);
} // namespace quantModeling

#endif
