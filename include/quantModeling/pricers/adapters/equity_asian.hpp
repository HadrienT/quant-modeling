#ifndef PRICERS_ADAPTERS_EQUITY_ASIAN_HPP
#define PRICERS_ADAPTERS_EQUITY_ASIAN_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{

    PricingResult price_equity_asian_bs(const AsianBSInput &in, EngineKind engine);
    /// The Monte-Carlo route: the Asian as a script (WP 26 P3).
    PricingResult price_equity_asian_bs_script(const AsianBSInput &in);

} // namespace quantModeling

#endif
