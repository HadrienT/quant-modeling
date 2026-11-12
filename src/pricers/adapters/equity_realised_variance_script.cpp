#include "quantModeling/pricers/adapters/equity_dispersion.hpp"
#include "quantModeling/pricers/adapters/equity_vol_swap.hpp"

#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

/**
 * @file equity_realised_variance_script.cpp
 * @brief Variance swap, volatility swap and dispersion swap as scripts
 *        (WP 26 P3): the hand-written engine's schedule (the observation
 *        dates given, else floor(252 T) evenly spaced ones) and its
 *        annualisation by the maturity; price only, as the engine.
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

        /// The engine's schedule as calendar days.
        std::vector<int> schedule(Time maturity, const std::vector<Time> &observations)
        {
            std::set<int> days;
            if (!observations.empty())
                for (Time t : observations)
                    days.insert(std::max(1, days_for(t)));
            else
            {
                const int n = std::max(1, static_cast<int>(252.0 * maturity));
                for (int i = 0; i < n; ++i)
                    days.insert(std::max(1, days_for(maturity * static_cast<Real>(i + 1) / static_cast<Real>(n))));
            }
            return {days.begin(), days.end()};
        }

        PricingResult realised(Real spot, Real rate, Real dividend, Real vol, Time maturity, Real strike, Real notional,
                               const std::vector<Time> &observations, int paths, int seed, bool volatility)
        {
            if (!(maturity > 0.0))
                throw InvalidInput(std::string(volatility ? "VolatilitySwap" : "VarianceSwap") +
                                   ": maturity must be positive");
            ScriptRoute route;
            route.valuation = script_builders::valuation();
            route.model.model = "black_scholes";
            route.model.spot = spot;
            route.model.rate = rate;
            route.model.dividend = dividend;
            route.model.vol = vol;
            const std::vector<int> dates = schedule(maturity, observations);
            const int pay = days_for(maturity);
            route.name = std::string(volatility ? "volatility swap, " : "variance swap, ") +
                         std::to_string(dates.size()) + " observations";
            route.script = [=](const scripting::ScriptModelSpec &m, int shift)
            {
                return script_builders::realised_swap(volatility, m.spot, strike, notional, maturity, dates,
                                                      std::max(dates.back(), pay + shift));
            };
            return price_by_script(route, mc_settings(paths, seed), false);
        }
    } // namespace

    PricingResult price_variance_swap_bs_mc(const VarianceSwapBSInput &in)
    {
        return realised(in.spot, in.rate, in.dividend, in.vol, in.maturity, in.strike_var, in.notional,
                        in.observation_dates, in.n_paths, in.seed, false);
    }

    PricingResult price_volatility_swap_bs_mc(const VolatilitySwapBSInput &in)
    {
        return realised(in.spot, in.rate, in.dividend, in.vol, in.maturity, in.strike_vol, in.notional,
                        in.observation_dates, in.n_paths, in.seed, true);
    }

    PricingResult price_dispersion_bs_mc(const DispersionBSInput &in)
    {
        if (!(in.maturity > 0.0))
            throw InvalidInput("DispersionSwap: maturity must be positive");
        const std::size_t n = in.spots.size();
        if (n < 2 || in.vols.size() != n || in.dividends.size() != n || in.weights.size() != n)
            throw InvalidInput("DispersionBSInput: spots, vols, dividends and weights need the same size, at least 2");
        if (!in.correlations.empty() && in.correlations.size() != n)
            throw InvalidInput("DispersionBSInput: correlations dimension mismatch");
        ScriptRoute route;
        route.valuation = script_builders::valuation();
        scripting::ScriptModelSpec &m = route.model;
        m.model = "black_scholes";
        m.rate = in.rate;
        m.spot = in.spots.front();
        m.spots.assign(in.spots.begin(), in.spots.end());
        m.vols.assign(in.vols.begin(), in.vols.end());
        m.dividends.assign(in.dividends.begin(), in.dividends.end());
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!in.correlations.empty() && in.correlations[i].size() != n)
                throw InvalidInput("DispersionBSInput: correlations not square");
            for (std::size_t j = 0; j < n; ++j)
                m.correlation.push_back(in.correlations.empty() ? (i == j ? 1.0 : 0.0) : in.correlations[i][j]);
        }
        const std::vector<int> dates = schedule(in.maturity, in.observation_dates);
        const int pay = days_for(in.maturity);
        route.name = "dispersion swap on " + std::to_string(n) + " assets, " + std::to_string(dates.size()) +
                     " observations";
        route.script = [in, dates, pay](const scripting::ScriptModelSpec &spec, int shift)
        {
            return script_builders::dispersion(spec.spots, in.weights, in.strike_spread, in.notional, in.maturity, dates,
                                               std::max(dates.back(), pay + shift));
        };
        return price_by_script(route, mc_settings(in.n_paths, in.seed), false);
    }

} // namespace quantModeling
