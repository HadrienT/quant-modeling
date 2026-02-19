#ifndef PRICERS_ADAPTERS_BONDS_HPP
#define PRICERS_ADAPTERS_BONDS_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_zero_coupon_bond_flat(const ZeroCouponBondInput &in);
    PricingResult price_fixed_rate_bond_flat(const FixedRateBondInput &in);

} // namespace quantModeling

#endif
