#include "quantModeling/pricers/adapters/equity_asian.hpp"
#include "quantModeling/pricers/adapters/equity_asian_lv.hpp"
#include "quantModeling/pricers/adapters/equity_lookback.hpp"
#include "quantModeling/pricers/adapters/equity_lookback_lv.hpp"

#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

/**
 * @file equity_asian_lookback_script.cpp
 * @brief The Asian and lookback routes as scripts (WP 26 P3): the fixings of
 *        the hand-written engines (round(252 T) evenly spaced dates, or the
 *        lookback's n_steps), Black-Scholes or local vol, the arithmetic
 *        Asian under Black-Scholes with the geometric Asian of its fixings as
 *        an analytic control.
 */
namespace quantModeling
{
    namespace
    {
        int daily_fixings(Time maturity)
        {
            return std::max(1, static_cast<int>(std::lround(252.0 * maturity)));
        }

        PricingSettings mc_settings(int paths, int seed)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            s.mc_device = ComputeDevice::Auto; // the GPU when present, the CPU otherwise
            return s;
        }

        scripting::ScriptModelSpec bs(Real spot, Real rate, Real dividend, Real vol)
        {
            scripting::ScriptModelSpec m;
            m.model = "black_scholes";
            m.spot = spot;
            m.rate = rate;
            m.dividend = dividend;
            m.vol = vol;
            return m;
        }

        /// Local vol stepping on the fixings: the hand-written engines' grid.
        scripting::ScriptModelSpec lv(Real spot, Real rate, Real dividend, const LocalVolSurface &s, Time maturity,
                                      int fixings)
        {
            scripting::ScriptModelSpec m;
            m.model = "local_vol";
            m.spot = spot;
            m.rate = rate;
            m.dividend = dividend;
            m.K_grid = s.K_grid;
            m.T_grid = s.T_grid;
            m.sigma_loc_flat = s.sigma_loc_flat;
            m.max_dt = maturity / fixings;
            return m;
        }

        std::vector<double> times_of(const std::vector<int> &days)
        {
            std::vector<double> t;
            for (int d : days)
                t.push_back(static_cast<double>(d) / 365.0);
            return t;
        }

        ScriptRoute asian_route(const script_builders::AsianTerms &terms, Time maturity, int n,
                                scripting::ScriptModelSpec model)
        {
            ScriptRoute route;
            route.valuation = script_builders::valuation();
            route.model = std::move(model);
            const int days = days_for(maturity);
            route.script = [terms, days, n](const scripting::ScriptModelSpec &, int shift)
            { return script_builders::asian(terms, script_builders::evenly(days + shift, n)); };
            route.name = std::string(terms.geometric ? "geometric" : "arithmetic") + " Asian " +
                         (terms.is_call ? "call" : "put") + ", " + std::to_string(n) + " fixings";
            if (terms.geometric_control)
                route.control_mean = [terms, days, n](const scripting::ScriptModelSpec &m, int shift)
                {
                    return std::optional<double>(script_builders::geometric_asian_bs(
                        m.spot, m.rate, m.dividend, m.vol, terms.strike, terms.is_call,
                        times_of(script_builders::evenly(days + shift, n))));
                };
            return route;
        }

        ScriptRoute lookback_route(const script_builders::LookbackTerms &terms, Time maturity, int n,
                                   scripting::ScriptModelSpec model)
        {
            ScriptRoute route;
            route.valuation = script_builders::valuation();
            route.model = std::move(model);
            const int days = days_for(maturity);
            // The extremum starts at the initial spot, a number in the script:
            // the greeks are central differences, so that the bumped runs see
            // the bumped spot.
            route.script = [terms, days, n](const scripting::ScriptModelSpec &m, int shift)
            { return script_builders::lookback(terms, m.spot, script_builders::evenly(days + shift, n)); };
            route.fd_greeks = true;
            route.name = std::string(terms.floating ? "floating" : "fixed") + "-strike lookback " +
                         (terms.is_call ? "call" : "put") + ", " + std::to_string(n) + " fixings";
            return route;
        }

        script_builders::LookbackTerms lookback_terms(bool is_call, Real strike, LookbackStyle style,
                                                      LookbackExtremum extremum)
        {
            script_builders::LookbackTerms t;
            t.is_call = is_call;
            t.strike = strike;
            t.floating = style == LookbackStyle::FloatingStrike;
            t.on_maximum = extremum == LookbackExtremum::Maximum;
            return t;
        }
    } // namespace

    PricingResult price_equity_asian_bs_script(const AsianBSInput &in)
    {
        script_builders::AsianTerms terms;
        terms.is_call = in.is_call;
        terms.strike = in.strike;
        terms.geometric = in.average_type == AsianAverageType::Geometric;
        terms.geometric_control = !terms.geometric;
        const ScriptRoute route = asian_route(terms, in.maturity, daily_fixings(in.maturity),
                                              bs(in.spot, in.rate, in.dividend, in.vol));
        return price_by_script(route, mc_settings(in.n_paths, in.seed), true);
    }

    PricingResult price_equity_asian_lv_mc(const AsianLocalVolInput &in)
    {
        script_builders::AsianTerms terms;
        terms.is_call = in.is_call;
        terms.strike = in.strike;
        terms.geometric = in.average_type == AsianAverageType::Geometric;
        const int n = daily_fixings(in.maturity);
        const ScriptRoute route =
            asian_route(terms, in.maturity, n, lv(in.spot, in.rate, in.dividend, in.surface, in.maturity, n));
        return price_by_script(route, mc_settings(in.n_paths, in.seed), true);
    }

    PricingResult price_equity_lookback_bs_mc(const LookbackBSInput &in)
    {
        const int n = in.n_steps > 0 ? in.n_steps : daily_fixings(in.maturity);
        const ScriptRoute route = lookback_route(lookback_terms(in.is_call, in.strike, in.style, in.extremum),
                                                 in.maturity, n, bs(in.spot, in.rate, in.dividend, in.vol));
        return price_by_script(route, mc_settings(in.n_paths, in.seed), true);
    }

    PricingResult price_equity_lookback_lv_mc(const LookbackLocalVolInput &in)
    {
        const int n = in.n_steps > 0 ? in.n_steps : daily_fixings(in.maturity);
        const ScriptRoute route =
            lookback_route(lookback_terms(in.is_call, in.strike, in.style, in.extremum), in.maturity, n,
                           lv(in.spot, in.rate, in.dividend, in.surface, in.maturity, n));
        return price_by_script(route, mc_settings(in.n_paths, in.seed), true);
    }

} // namespace quantModeling
