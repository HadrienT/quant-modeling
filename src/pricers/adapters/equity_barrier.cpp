#include "quantModeling/pricers/adapters/equity_barrier.hpp"

#include "quantModeling/engines/mc/barrier.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_barrier_bs_mc(const BarrierBSInput &in)
    {
        // Build the vanilla underlying payoff (call or put)
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        // European exercise
        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        // Build the barrier option instrument
        BarrierOption opt(payoff, exercise,
                          in.barrier_type,
                          static_cast<Real>(in.barrier_level),
                          static_cast<Real>(in.rebate),
                          1.0 /* notional, scaled below */);
        opt.n_steps = in.n_steps;
        opt.brownian_bridge = in.brownian_bridge;

        // Black-Scholes model
        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        settings.mc_antithetic = false; // CRN Greek scheme; antithetic handled separately

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSEuroBarrierMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
