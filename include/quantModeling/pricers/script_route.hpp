#ifndef QM_PRICERS_SCRIPT_ROUTE_HPP
#define QM_PRICERS_SCRIPT_ROUTE_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/results.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <functional>
#include <optional>
#include <string>

namespace quantModeling
{

    /**
     * @brief A catalogue product priced as a script by the generic engine
     *        (blueprint/wp/26-performance.md §7, WP 26 P3).
     *
     * The product's terms become a script (a C++ script builder per route,
     * pricers/adapters/), its model a ScriptModelSpec; price_by_script()
     * prices it and fills the PricingResult the hand-written engine filled.
     */
    struct ScriptRoute
    {
        /// The script, its maturity moved by `days` calendar days (theta's
        /// bumps; 0 for the price). Every other term stays.
        std::function<std::string(int days)> script;
        /// The valuation date the script's dates count from.
        Date valuation;
        /// The model at the request's inputs.
        scripting::ScriptModelSpec model;
        /// The expectation of the script's controlPays cash flows under a
        /// model and a maturity shift, when the route has an analytic control
        /// (pricers/script_route.hpp; WP 26 P2): the geometric Asian of an
        /// arithmetic one. Empty: no control.
        std::function<std::optional<double>(const scripting::ScriptModelSpec &, int days)> control_mean;
        /// The engine's name in the diagnostics (e.g. "barrier").
        std::string name;
        /// Fuzzy (smoothed) ifs.
        bool fuzzy = false;
        /// The payoff jumps on a path (a barrier checked on dates, a
        /// digital): its pathwise derivatives miss the jump, so delta, vega
        /// and rho are central differences with common random numbers too,
        /// as gamma and theta, instead of the adjoint.
        bool discontinuous = false;
    };

    /// The calendar days from `valuation` that put a date at year fraction
    /// `t` under ACT/365F, the scripts' day count: exact for a maturity the
    /// API computed from dates, rounded to the nearest day otherwise.
    int days_for(Time t);

    /**
     * @brief Price a route; with `greeks`, its five greeks.
     *
     * Price: the script engine (settings.mc_device; Sobol by default), with
     * the route's analytic control when it has one. Delta, vega and rho (a
     * route that is not `discontinuous`; otherwise central differences): one
     * adjoint run on the CPU tape (the model's spot, flat or local vol --
     * every grid point, so a parallel shift -- and rate sensitivities summed
     * per greek, their errors from the adjoint's batches). Gamma: central
     * second difference of the price in the spot (±1 %), theta: -dV/dT by
     * moving the maturity one day each way, both with common random numbers
     * (the same Sobol points) and their errors from the replicate-wise
     * differences. Units as the hand-written engines: vega per unit of
     * volatility, theta per year, rho per unit of rate.
     */
    PricingResult price_by_script(const ScriptRoute &route, const PricingSettings &settings, bool greeks);

} // namespace quantModeling

#endif // QM_PRICERS_SCRIPT_ROUTE_HPP
