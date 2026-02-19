#include "quantModeling/pricers/adapters/equity_vanilla_american.hpp"

#include "quantModeling/engines/tree/binomial.hpp"
#include "quantModeling/engines/tree/trinomial.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{
    PricingResult price_equity_vanilla_american_bs(const AmericanVanillaBSInput &in, EngineKind engine)
    {
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
