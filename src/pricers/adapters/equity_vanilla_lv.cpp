#include "quantModeling/pricers/adapters/equity_vanilla_lv.hpp"

#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace quantModeling
{

    PricingResult price_local_vol_mc(const LocalVolInput &in)
    {
        if (in.K_grid.empty() || in.T_grid.empty() || in.sigma_loc_flat.empty())
            throw std::invalid_argument("price_local_vol_mc: local-vol grid is empty.");
        if (in.sigma_loc_flat.size() != in.K_grid.size() * in.T_grid.size())
            throw std::invalid_argument(
                "price_local_vol_mc: sigma_loc_flat.size() != K_grid.size() * T_grid.size().");
        if (!(in.maturity > 0.0))
            throw std::invalid_argument("price_local_vol_mc: maturity must be > 0.");
        if (in.n_steps_per_year <= 0)
            throw std::invalid_argument("price_local_vol_mc: n_steps_per_year must be > 0.");

        scripting::ScriptModelSpec m;
        m.model = "local_vol";
        m.spot = in.spot;
        m.rate = in.rate;
        m.dividend = in.dividend;
        m.K_grid = in.K_grid;
        m.T_grid = in.T_grid;
        m.sigma_loc_flat = in.sigma_loc_flat;
        const int steps =
            std::max(static_cast<int>(std::ceil(in.maturity * static_cast<Real>(in.n_steps_per_year))), 1);
        m.max_dt = in.maturity / steps;

        ScriptRoute route;
        route.valuation = script_builders::valuation();
        route.maturity = in.maturity;
        route.model = std::move(m);
        route.name = std::string("local-vol vanilla ") + (in.is_call ? "call" : "put") + ", " +
                     std::to_string(steps) + " steps";
        const int days = ticks_for(in.maturity);
        const bool is_call = in.is_call;
        const Real strike = in.strike;
        route.script = [is_call, strike, days](const scripting::ScriptModelSpec &, int shift)
        { return script_builders::european(is_call, strike, days + shift); };

        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        settings.mc_device = ComputeDevice::Auto; // the GPU when present and the script fits, the CPU otherwise
        return price_by_script(route, settings, in.compute_greeks);
    }

} // namespace quantModeling
