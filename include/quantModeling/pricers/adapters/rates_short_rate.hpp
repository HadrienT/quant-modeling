#ifndef PRICERS_ADAPTERS_RATES_SHORT_RATE_HPP
#define PRICERS_ADAPTERS_RATES_SHORT_RATE_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    // ── ZCB / FixedRateBond under short-rate models ──────────────────────────

    PricingResult price_zcb_short_rate(const struct ShortRateZCBInput &in);
    PricingResult price_fixed_bond_short_rate(const struct ShortRateBondInput &in);

    // ── Bond option under short-rate models ──────────────────────────────────

    PricingResult price_bond_option_short_rate_analytic(const struct ShortRateBondOptionInput &in);
    PricingResult price_bond_option_short_rate_mc(const struct ShortRateBondOptionInput &in);

    // ── Cap / Floor under short-rate models ──────────────────────────────────

    PricingResult price_capfloor_short_rate_analytic(const struct ShortRateCapFloorInput &in);
    PricingResult price_capfloor_short_rate_mc(const struct ShortRateCapFloorInput &in);

    // ── Caplet / Floorlet under short-rate models ────────────────────────────

    PricingResult price_caplet_short_rate_analytic(const struct ShortRateCapletInput &in);
    PricingResult price_caplet_short_rate_mc(const struct ShortRateCapletInput &in);

} // namespace quantModeling

#endif
