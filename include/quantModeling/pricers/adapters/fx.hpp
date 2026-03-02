#ifndef ADAPTERS_FX_HPP
#define ADAPTERS_FX_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_fx_forward_analytic(const FXForwardInput &in);
    PricingResult price_fx_option_analytic(const FXOptionInput &in);

} // namespace quantModeling

#endif
