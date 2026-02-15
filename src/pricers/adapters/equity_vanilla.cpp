#include "quantModeling/pricers/adapters/equity_vanilla.hpp"

#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_vanilla_bs(const VanillaBSInput &in, EngineKind engine)
    {
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        VanillaOption opt(payoff, exercise, 1.0);

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        MarketView market = {};
        const bool use_mc = engine == EngineKind::MonteCarlo;
        PricingSettings settings = {
            use_mc ? in.n_paths : 0,
            use_mc ? in.seed : 0,
            in.mc_epsilon};

        PricingContext ctx{market, settings, model};

        if (use_mc)
        {
            BSEuroVanillaMCEngine mc_engine(ctx);
            return price(opt, mc_engine);
        }

        BSEuroVanillaAnalyticEngine analytic_engine(ctx);
        return price(opt, analytic_engine);
    }

} // namespace quantModeling
