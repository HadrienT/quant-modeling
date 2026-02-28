#include "quantModeling/pricers/adapters/equity_lookback.hpp"

#include "quantModeling/engines/mc/lookback.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_lookback_bs_mc(const LookbackBSInput &in)
    {
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        LookbackOption opt(payoff, exercise, in.style, in.extremum, 1.0);
        opt.n_steps = in.n_steps;

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        settings.mc_antithetic = in.mc_antithetic;

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSEuroLookbackMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
