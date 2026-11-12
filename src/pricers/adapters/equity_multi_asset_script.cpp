#include "quantModeling/pricers/adapters/equity_autocall.hpp"
#include "quantModeling/pricers/adapters/equity_basket.hpp"
#include "quantModeling/pricers/adapters/equity_mountain.hpp"
#include "quantModeling/pricers/adapters/equity_rainbow.hpp"

#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <string>
#include <vector>

/**
 * @file equity_multi_asset_script.cpp
 * @brief Worst-of, best-of, basket, mountain and autocall as scripts
 *        (WP 26 P3) under (correlated) Black-Scholes: the hand-written
 *        engines' payoffs and dates, the generic engine's paths.
 */
namespace quantModeling
{
    namespace
    {
        PricingSettings mc_settings(int paths, int seed)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            s.mc_device = ComputeDevice::Auto; // the GPU when present, the CPU otherwise
            return s;
        }

        /// Correlated Black-Scholes; an empty correlation is the identity.
        scripting::ScriptModelSpec multi_bs(Real rate, const std::vector<Real> &spots, const std::vector<Real> &vols,
                                            const std::vector<Real> &dividends,
                                            const std::vector<std::vector<Real>> &correlations)
        {
            const std::size_t n = spots.size();
            if (n < 2 || vols.size() != n || dividends.size() != n)
                throw InvalidInput("multi-asset Black-Scholes: spots, vols and dividends need the same size, at least 2");
            if (!correlations.empty() && correlations.size() != n)
                throw InvalidInput("multi-asset Black-Scholes: correlations must be n x n or empty");
            scripting::ScriptModelSpec m;
            m.model = "black_scholes";
            m.rate = rate;
            m.spot = spots.front();
            m.spots.assign(spots.begin(), spots.end());
            m.vols.assign(vols.begin(), vols.end());
            m.dividends.assign(dividends.begin(), dividends.end());
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                {
                    if (!correlations.empty() && correlations[i].size() != n)
                        throw InvalidInput("multi-asset Black-Scholes: correlations row size mismatch");
                    m.correlation.push_back(correlations.empty() ? (i == j ? 1.0 : 0.0) : correlations[i][j]);
                }
            return m;
        }

        std::vector<int> days_of(const std::vector<Time> &times)
        {
            std::vector<int> d;
            for (Time t : times)
                d.push_back(ticks_for(t));
            return d;
        }

        ScriptRoute route_of(std::string name, scripting::ScriptModelSpec model,
                             std::function<std::string(const scripting::ScriptModelSpec &, int)> script,
                             Time maturity)
        {
            ScriptRoute r;
            r.valuation = script_builders::valuation();
            r.maturity = maturity;
            r.model = std::move(model);
            r.name = std::move(name);
            r.script = std::move(script);
            return r;
        }

        PricingResult rainbow(const RainbowBSInput &in, bool worst)
        {
            const int days = ticks_for(in.maturity);
            const ScriptRoute route = route_of(
                std::string(worst ? "worst-of " : "best-of ") + (in.is_call ? "call" : "put") + " on " +
                    std::to_string(in.spots.size()) + " assets",
                multi_bs(in.rate, in.spots, in.vols, in.dividends, in.correlations),
                [in, worst, days](const scripting::ScriptModelSpec &m, int shift)
                { return script_builders::rainbow(worst, in.is_call, in.strike, in.notional, m.spots, days + shift); },
                in.maturity);
            return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
        }
    } // namespace

    PricingResult price_worst_of_bs_mc(const RainbowBSInput &in)
    {
        return rainbow(in, true);
    }
    PricingResult price_best_of_bs_mc(const RainbowBSInput &in)
    {
        return rainbow(in, false);
    }

    PricingResult price_equity_basket_bs_mc(const BasketBSInput &in)
    {
        if (in.weights.size() != in.spots.size())
            throw InvalidInput("BasketBSInput: weights.size() != spots.size()");
        const int days = ticks_for(in.maturity);
        ScriptRoute route = route_of(
            std::string("basket ") + (in.is_call ? "call" : "put") + " on " + std::to_string(in.spots.size()) +
                " assets",
            multi_bs(in.rate, in.spots, in.vols, in.dividends, in.correlations),
            [in, days](const scripting::ScriptModelSpec &, int shift)
            { return script_builders::basket(in.is_call, in.strike, in.weights, days + shift); }, in.maturity);
        // Delta and gamma per unit of the basket's initial value, every spot
        // scaled (the hand-written engine's convention).
        Real b0 = 0.0;
        for (std::size_t i = 0; i < in.spots.size(); ++i)
            b0 += in.weights[i] * in.spots[i];
        route.spot_reference = b0;
        return price_by_script(route, mc_settings(in.n_paths, in.seed), true);
    }

    PricingResult price_equity_mountain_bs_mc(const MountainBSInput &in)
    {
        if (in.observation_dates.size() != in.spots.size())
            throw InvalidInput("MountainBSInput: one observation date per asset");
        const std::vector<int> dates = days_of(in.observation_dates);
        const ScriptRoute route = route_of(
            std::string("mountain ") + (in.is_call ? "call" : "put") + " on " + std::to_string(in.spots.size()) +
                " assets",
            multi_bs(in.rate, in.spots, in.vols, in.dividends, in.correlations),
            [in, dates](const scripting::ScriptModelSpec &m, int shift)
            {
                std::vector<int> d = dates;
                d.back() += shift;
                return script_builders::mountain(in.is_call, in.strike, in.notional, m.spots, d);
            },
            in.observation_dates.back());
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

    PricingResult price_equity_autocall_bs_mc(const AutocallBSInput &in)
    {
        if (in.observation_dates.empty())
            throw InvalidInput("AutocallBSInput: no observation date");
        scripting::ScriptModelSpec m;
        m.model = "black_scholes";
        m.spot = in.spot;
        m.rate = in.rate;
        m.dividend = in.dividend;
        m.vol = in.vol;
        const std::vector<int> dates = days_of(in.observation_dates);
        const ScriptRoute route = route_of(
            "autocall, " + std::to_string(dates.size()) + " observations", std::move(m),
            [in, dates](const scripting::ScriptModelSpec &spec, int shift)
            {
                script_builders::AutocallTerms t;
                t.s0 = spec.spot;
                t.autocall_barrier = in.autocall_barrier;
                t.coupon_barrier = in.coupon_barrier;
                t.put_barrier = in.put_barrier;
                t.coupon_rate = in.coupon_rate;
                t.notional = in.notional;
                t.memory_coupon = in.memory_coupon;
                t.ki_continuous = in.ki_continuous;
                std::vector<int> d = dates;
                d.back() += shift;
                return script_builders::autocall(t, d);
            },
            in.observation_dates.back());
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

} // namespace quantModeling
