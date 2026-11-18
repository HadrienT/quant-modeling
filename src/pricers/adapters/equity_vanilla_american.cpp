#include "quantModeling/pricers/adapters/equity_vanilla_american.hpp"

#include "quantModeling/engines/tree/binomial.hpp"
#include "quantModeling/engines/tree/trinomial.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"
#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/pricers/script_route.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace quantModeling
{
    namespace
    {
        /// Monte-Carlo (#313): a Bermudan on exercise_per_year dates a year
        /// standing for continuous exercise, the rule by Longstaff & Schwartz
        /// (script_builders::american). A call with no dividend (and a
        /// non-negative rate) is never exercised early (Merton 1973): it is
        /// the European, priced as such -- exact, where an estimated rule
        /// would only lose value by exercising on noise.
        PricingResult price_american_mc(const AmericanVanillaBSInput &in)
        {
            if (in.exercise_per_year < 1)
                throw InvalidInput("American Monte-Carlo: exercise_per_year must be at least 1");
            const bool never_early = in.is_call && in.dividend <= 0.0 && in.rate >= 0.0;
            ScriptRoute route;
            route.valuation = script_builders::valuation();
            route.maturity = in.maturity;
            route.model.model = "black_scholes";
            route.model.spot = in.spot;
            route.model.rate = in.rate;
            route.model.dividend = in.dividend;
            route.model.vol = in.vol;
            const int days = ticks_for(in.maturity);
            const int dates = std::max(1, static_cast<int>(std::lround(in.exercise_per_year * in.maturity)));
            const bool is_call = in.is_call;
            const Real strike = in.strike;
            if (never_early)
            {
                route.name = "American call without dividend = European (Merton 1973)";
                route.script = [is_call, strike, days](const scripting::ScriptModelSpec &, int shift)
                { return script_builders::european(is_call, strike, days + shift); };
            }
            else
            {
                route.name = std::string("American ") + (is_call ? "call" : "put") + ", " + std::to_string(dates) +
                             " exercise dates, Longstaff-Schwartz";
                const Real q = in.dividend;
                const Time maturity = in.maturity;
                route.script = [is_call, strike, days, dates, q, maturity](const scripting::ScriptModelSpec &, int shift)
                {
                    const Time T = maturity * static_cast<double>(days + shift) / static_cast<double>(days);
                    return script_builders::american(is_call, strike, script_builders::evenly(days + shift, dates), q,
                                                     T);
                };
            }
            PricingSettings settings;
            settings.mc_paths = in.n_paths;
            settings.mc_seed = in.seed;
            settings.mc_device = ComputeDevice::Auto; // the GPU when present and the script fits, the CPU otherwise
            settings.mc_threads = in.threads;
            return price_by_script(route, settings, in.compute_greeks);
        }
    } // namespace

    PricingResult price_equity_vanilla_american_bs(const AmericanVanillaBSInput &in, EngineKind engine)
    {
        if (engine == EngineKind::MonteCarlo)
            return price_american_mc(in);

        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<AmericanExercise>(
            static_cast<Real>(in.maturity));

        VanillaOption opt(payoff, exercise, 1.0);

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        MarketView market = {};
        PricingSettings settings = {
            0,    // mc_paths
            0,    // mc_seed
            true, // mc_antithetic
            in.tree_steps,
            in.pde_space_steps,
            in.pde_time_steps};
        PricingContext ctx{market, settings, model};

        switch (engine)
        {
            case EngineKind::BinomialTree:
            {
                BinomialVanillaEngine binomial_engine(ctx);
                return price(opt, binomial_engine);
            }
            case EngineKind::TrinomialTree:
            {
                TrinomialVanillaEngine trinomial_engine(ctx);
                return price(opt, trinomial_engine);
            }
            case EngineKind::PDEFiniteDifference:
            {
                throw UnsupportedInstrument("PDE finite difference method is only supported for European vanilla options");
            }
            default:
                throw InvalidInput("Unsupported engine for American vanilla options");
        }
    }
} // namespace quantModeling
