#include "quantModeling/pricers/adapters/equity_barrier.hpp"
#include "quantModeling/pricers/adapters/equity_barrier_lv.hpp"

#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{
    namespace
    {
        script_builders::BarrierTerms terms_of(bool is_call, Real strike, BarrierType type, Real level, Real rebate)
        {
            script_builders::BarrierTerms t;
            t.is_call = is_call;
            t.strike = strike;
            t.type = type;
            t.level = level;
            t.rebate = rebate;
            return t;
        }

        /// Continuous monitoring (the bridge on) reads the barrier through
        /// the model's survival probabilities; without the bridge, the
        /// barrier is checked on the dates of the hand-written engine's grid
        /// (n_steps, or `per_year` a year).
        PricingResult price_barrier(const script_builders::BarrierTerms &terms, Time maturity, int n_steps,
                                    bool bridge, int per_year, scripting::ScriptModelSpec model, int paths, int seed,
                                    bool greeks)
        {
            const int observations =
                bridge ? 0 : (n_steps > 0 ? n_steps : std::max(1, static_cast<int>(std::lround(per_year * maturity))));
            ScriptRoute route;
            route.valuation = script_builders::valuation();
            route.model = std::move(model);
            const char *kind[] = {"down-and-out", "down-and-in", "up-and-out", "up-and-in"};
            const int k = terms.type == BarrierType::DownAndOut  ? 0
                          : terms.type == BarrierType::DownAndIn ? 1
                          : terms.type == BarrierType::UpAndOut  ? 2
                                                                 : 3;
            route.name = std::string(kind[k]) + (terms.is_call ? " call" : " put") + ", barrier " +
                         script_builders::number(terms.level) +
                         (bridge ? ", continuous" : ", " + std::to_string(observations) + " dates");
            route.fd_greeks = !bridge; // a hard knock on dates: pathwise derivatives miss the jump
            const int days = days_for(maturity);
            route.script = [terms, days, observations](const scripting::ScriptModelSpec &, int shift)
            { return script_builders::barrier(terms, days + shift, observations); };
            PricingSettings settings;
            settings.mc_paths = paths;
            settings.mc_seed = seed;
            settings.mc_device = ComputeDevice::Auto; // the GPU when present and the script fits, the CPU otherwise
            return price_by_script(route, settings, greeks);
        }
    } // namespace

    PricingResult price_equity_barrier_bs_mc(const BarrierBSInput &in)
    {
        scripting::ScriptModelSpec m;
        m.model = "black_scholes";
        m.spot = in.spot;
        m.rate = in.rate;
        m.dividend = in.dividend;
        m.vol = in.vol;
        return price_barrier(terms_of(in.is_call, in.strike, in.barrier_type, in.barrier_level, in.rebate),
                             in.maturity, in.n_steps, in.brownian_bridge, 52, std::move(m), in.n_paths, in.seed,
                             in.compute_greeks);
    }

    PricingResult price_equity_barrier_lv_mc(const BarrierLocalVolInput &in)
    {
        scripting::ScriptModelSpec m;
        m.model = "local_vol";
        m.spot = in.spot;
        m.rate = in.rate;
        m.dividend = in.dividend;
        m.K_grid = in.surface.K_grid;
        m.T_grid = in.surface.T_grid;
        m.sigma_loc_flat = in.surface.sigma_loc_flat;
        // The survivals are taken on the model's steps: the hand-written
        // engine's grid (n_steps, or 52 a year).
        const int steps = in.n_steps > 0 ? in.n_steps : std::max(1, static_cast<int>(std::lround(52.0 * in.maturity)));
        m.max_dt = in.maturity / steps;
        return price_barrier(terms_of(in.is_call, in.strike, in.barrier_type, in.barrier_level, in.rebate),
                             in.maturity, in.n_steps, in.brownian_bridge, 52, std::move(m), in.n_paths, in.seed,
                             in.compute_greeks);
    }

} // namespace quantModeling
