#ifndef ADAPTERS_EQUITY_VOL_SWAP_HPP
#define ADAPTERS_EQUITY_VOL_SWAP_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_variance_swap_bs_analytic(const VarianceSwapBSInput &in);
    PricingResult price_variance_swap_bs_mc(const VarianceSwapBSInput &in);
    PricingResult price_volatility_swap_bs_mc(const VolatilitySwapBSInput &in);

} // namespace quantModeling

#endif
