#include "quantModeling/pricers/adapters/rates_short_rate.hpp"

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <string>
#include <vector>

/**
 * @file rates_short_rate_script.cpp
 * @brief Bond option, caplet and cap/floor under the short-rate models as
 *        scripts (WP 26 P3) on ShortRateSimModel: the hand-written engine's
 *        Euler steps (at most 1/252 year), numeraire and closed-form bonds;
 *        prices only, as the engine.
 */
namespace quantModeling
{
    namespace
    {
        scripting::ScriptModelSpec short_rate(const std::string &type, Real a, Real b, Real sigma, Real r0)
        {
            scripting::ScriptModelSpec m;
            m.model = "short_rate";
            m.short_rate_type = type;
            m.sr_a = a;
            m.sr_b = b;
            m.sr_sigma = sigma;
            m.sr_r0 = r0;
            m.max_dt = 1.0 / 252.0;
            return m;
        }

        PricingSettings mc_settings(int paths, int seed)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            s.mc_device = ComputeDevice::Auto; // the CPU: the GPU has no short-rate model
            return s;
        }
    } // namespace

    PricingResult price_bond_option_short_rate_mc(const ShortRateBondOptionInput &in)
    {
        if (!(in.option_maturity > 0.0))
            throw InvalidInput("BondOption: option_maturity must be > 0");
        if (!(in.bond_maturity > in.option_maturity))
            throw InvalidInput("BondOption: bond_maturity must be > option_maturity");
        ScriptRoute route;
        route.valuation = script_builders::valuation();
        route.maturity = in.option_maturity;
        route.model = short_rate(in.model_type, in.a, in.b, in.sigma, in.r0);
        route.name = std::string("bond ") + (in.is_call ? "call" : "put") + " (" + in.model_type + ")";
        const int expiry = ticks_for(in.option_maturity), bond = ticks_for(in.bond_maturity);
        route.script = [in, expiry, bond](const scripting::ScriptModelSpec &, int shift)
        { return script_builders::bond_option(in.is_call, in.strike, in.notional, expiry + shift, bond); };
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

    PricingResult price_caplet_short_rate_mc(const ShortRateCapletInput &in)
    {
        if (!(in.start > 0.0))
            throw InvalidInput("Caplet: start must be > 0");
        if (!(in.end > in.start))
            throw InvalidInput("Caplet: end must be > start");
        ScriptRoute route;
        route.valuation = script_builders::valuation();
        route.maturity = in.start;
        route.model = short_rate(in.model_type, in.a, in.b, in.sigma, in.r0);
        route.name = std::string(in.is_cap ? "caplet" : "floorlet") + " (" + in.model_type + ")";
        const int reset = ticks_for(in.start), end = ticks_for(in.end);
        route.script = [in, reset, end](const scripting::ScriptModelSpec &, int shift)
        {
            return script_builders::caplets(in.is_cap, in.strike, in.notional, {reset + shift}, {end},
                                            {in.end - in.start});
        };
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

    PricingResult price_capfloor_short_rate_mc(const ShortRateCapFloorInput &in)
    {
        if (in.schedule.size() < 2)
            throw InvalidInput("CapFloor: schedule needs at least 2 dates");
        std::vector<int> resets, ends;
        std::vector<Real> deltas;
        for (std::size_t i = 0; i + 1 < in.schedule.size(); ++i)
            if (in.schedule[i] > 0.0) // expired resets are left out, as the engine did
            {
                resets.push_back(ticks_for(in.schedule[i]));
                ends.push_back(ticks_for(in.schedule[i + 1]));
                deltas.push_back(in.schedule[i + 1] - in.schedule[i]);
            }
        if (resets.empty())
        {
            PricingResult out;
            out.npv = 0.0;
            out.mc_std_error = 0.0;
            out.diagnostics = "CapFloor: every caplet has expired";
            return out;
        }
        ScriptRoute route;
        route.valuation = script_builders::valuation();
        route.maturity = in.schedule.back();
        route.model = short_rate(in.model_type, in.a, in.b, in.sigma, in.r0);
        route.name = std::string(in.is_cap ? "cap" : "floor") + " of " + std::to_string(resets.size()) + " periods (" +
                     in.model_type + ")";
        route.script = [in, resets, ends, deltas](const scripting::ScriptModelSpec &, int)
        { return script_builders::caplets(in.is_cap, in.strike, in.notional, resets, ends, deltas); };
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

} // namespace quantModeling
