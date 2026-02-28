#include "quantModeling/pricers/adapters/equity_asian_lv.hpp"

#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/dupire.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_asian_lv_mc(const AsianLocalVolInput &in)
    {
        std::shared_ptr<IPayoff> payoff;
        if (in.average_type == AsianAverageType::Arithmetic)
        {
            payoff = std::make_shared<ArithmeticAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<Real>(in.strike));
        }
        else
        {
            payoff = std::make_shared<GeometricAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<Real>(in.strike));
        }

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        AsianOption opt(payoff, exercise, in.average_type, 1.0);

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
        settings.mc_antithetic = in.mc_antithetic;

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSEuroAsianMCEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
