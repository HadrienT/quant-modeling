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
        /// The script under a model (a product whose script carries a model
        /// input, as the lookback's initial spot, reads it here, so that the
        /// bumped runs see the bumped input), its maturity moved by `days`
        /// calendar days (theta's bumps; 0 for the price). Every other term
        /// stays.
        std::function<std::string(const scripting::ScriptModelSpec &, int days)> script;
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
        /// Delta, vega and rho by central differences with common random
        /// numbers too, as gamma and theta, instead of the adjoint: for a
        /// payoff that jumps on a path (a barrier checked on dates, a
        /// digital), whose pathwise derivatives miss the jump, and for a
        /// script that carries a model input as a number (the adjoint does
        /// not see it move).
        bool fd_greeks = false;
        /// Several underlyings (ScriptModelSpec::spots): delta and gamma
        /// scale every spot by 1 ± 1 % and are per unit of this reference
        /// level (a basket's initial value), the hand-written basket engine's
        /// convention; 0 takes the first spot. Delta is then a central
        /// difference too.
        Real spot_reference = 0.0;
    };

    /// The calendar days from `valuation` that put a date at year fraction
    /// `t` under ACT/365F, the scripts' day count: exact for a maturity the
    /// API computed from dates, rounded to the nearest day otherwise.
    int days_for(Time t);

    /**
     * @brief Price a route; with `greeks`, its five greeks.
     *
     * Everything runs on settings.mc_device (the adapters ask for Auto: the
     * GPU when present and the script fits, the CPU otherwise), Sobol by
     * default. Price: the script engine, with the route's analytic control
     * when it has one. Delta, vega and rho (a route without `fd_greeks`):
     * one adjoint run -- on the GPU when it takes the request, else the CPU
     * tape -- the spot, volatility and rate sensitivities summed per greek,
     * their errors from the adjoint's batches; a greek that sums several
     * parameters on the GPU (local vol's vega) is a central difference
     * instead, since only the CPU tape gives the sum's error. Gamma, theta,
     * and the greeks of a route with `fd_greeks`: central differences with
     * common random numbers (the same Sobol points), their errors from the
     * replicate-wise differences. Bumps as utils/greeks.hpp; units as the
     * hand-written engines: vega per unit of volatility, theta per year,
     * rho per unit of rate.
     */
    PricingResult price_by_script(const ScriptRoute &route, const PricingSettings &settings, bool greeks);

} // namespace quantModeling

#endif // QM_PRICERS_SCRIPT_ROUTE_HPP
