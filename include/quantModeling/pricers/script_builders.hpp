#ifndef QM_PRICERS_SCRIPT_BUILDERS_HPP
#define QM_PRICERS_SCRIPT_BUILDERS_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"

#include <string>

/**
 * @file script_builders.hpp
 * @brief Catalogue products written as scripts (WP 26 P3,
 *        blueprint/wp/26-performance.md §7): the terms of a product in, the
 *        script the generic engine prices out. One builder per product
 *        family; the adapters (pricers/adapters/) pair it with a model and
 *        price it with price_by_script().
 *
 * Dates count from the route's valuation date in calendar days (ACT/365F,
 * the scripts' day count): `days` is the maturity, as days_for() gives it.
 * Numbers are written with 17 significant digits, so the script prices the
 * terms it was given to the bit.
 */
namespace quantModeling::script_builders
{

    /// The valuation date every route's dates count from. Any date works:
    /// the scripts only see differences of dates.
    Date valuation();

    /// A number as the script parser reads it back to the bit.
    std::string number(double x);

    struct BarrierTerms
    {
        bool is_call = true;
        Real strike = 0.0;
        BarrierType type = BarrierType::DownAndOut;
        Real level = 0.0;
        Real rebate = 0.0; ///< paid at expiry in the "dead" state (knocked out, or never knocked in)
    };

    /**
     * Knock-in / knock-out vanilla. `observations` = 0: continuous
     * monitoring, the barrier read through survivalAbove / survivalBelow
     * (WP 26 P2) -- exact under Black-Scholes, per model step under local
     * vol. `observations` > 0: the spot checked on that many evenly spaced
     * dates up to the expiry (the hand-written engine's discrete mode).
     */
    std::string barrier(const BarrierTerms &t, int days, int observations = 0);

} // namespace quantModeling::script_builders

#endif // QM_PRICERS_SCRIPT_BUILDERS_HPP
