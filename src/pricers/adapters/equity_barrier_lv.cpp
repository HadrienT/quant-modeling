#include "quantModeling/pricers/adapters/equity_barrier_lv.hpp"

#include "quantModeling/engines/mc/barrier.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/dupire.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_barrier_lv_mc(const BarrierLocalVolInput &in)
    {
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        BarrierOption opt(payoff, exercise,
                          in.barrier_type,
                          static_cast<Real>(in.barrier_level),
                          static_cast<Real>(in.rebate),
                          1.0);
        opt.n_steps = in.n_steps;
        opt.brownian_bridge = in.brownian_bridge;

        auto model = std::make_shared<DupireModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            in.surface.K_grid,
            in.surface.T_grid,
            in.surface.sigma_loc_flat);

        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        settings.mc_antithetic = false; // CRN Greek scheme

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSEuroBarrierMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
