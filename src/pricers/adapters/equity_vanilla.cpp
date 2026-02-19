#include "quantModeling/pricers/adapters/equity_vanilla.hpp"

#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/engines/pde/european_vanilla.hpp"
#include "quantModeling/engines/tree/binomial.hpp"
#include "quantModeling/engines/tree/trinomial.hpp"
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
        const bool use_pde = engine == EngineKind::PDEFiniteDifference;
        const bool use_binomial = engine == EngineKind::BinomialTree;
        const bool use_trinomial = engine == EngineKind::TrinomialTree;
        PricingSettings settings = {
            use_mc ? in.n_paths : 0,
            use_mc ? in.seed : 0,
            true,
            in.tree_steps,
            in.pde_space_steps,
            in.pde_time_steps};

        PricingContext ctx{market, settings, model};

        if (use_mc)
        {
            BSEuroVanillaMCEngine mc_engine(ctx);
            return price(opt, mc_engine);
        }
        else if (use_pde)
        {
            PDEEuropeanVanillaEngine pde_engine(ctx);
            return price(opt, pde_engine);
        }
        else if (use_binomial)
        {
            BinomialVanillaEngine binomial_engine(ctx);
            return price(opt, binomial_engine);
        }
        else if (use_trinomial)
        {
            TrinomialVanillaEngine trinomial_engine(ctx);
            return price(opt, trinomial_engine);
        }

        BSEuroVanillaAnalyticEngine analytic_engine(ctx);
        return price(opt, analytic_engine);
    }

} // namespace quantModeling
