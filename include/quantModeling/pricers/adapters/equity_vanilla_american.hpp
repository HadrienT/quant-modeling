#ifndef PRICERS_ADAPTERS_EQUITY_VANILLA_AMERICAN_HPP
#define PRICERS_ADAPTERS_EQUITY_VANILLA_AMERICAN_HPP

#include "quantModeling/pricers/registry.hpp"

namespace quantModeling
{
    /**
     * @brief Price American vanilla options using Black-Scholes model
     *
     * Supports four numerical methods:
     * - BinomialTree: Cox-Ross-Rubinstein binomial tree
     * - TrinomialTree: Boyle's trinomial tree
     * - PDEFiniteDifference: Crank-Nicolson finite difference scheme
     * - MonteCarlo: Longstaff-Schwartz on a script (a Bermudan on
     *   exercise_per_year dates a year), the route the site prices (#313);
     *   the trees stay as its oracle
     */
    PricingResult price_equity_vanilla_american_bs(const AmericanVanillaBSInput &in, EngineKind engine);

} // namespace quantModeling

#endif
