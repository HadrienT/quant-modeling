#include "quantModeling/pricers/adapters/equity_digital.hpp"

#include "quantModeling/engines/analytic/digital.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_digital_bs_analytic(const DigitalBSInput &in)
    {
        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<Real>(in.strike));

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        DigitalOption opt(payoff, exercise,
                          in.payoff_type,
                          static_cast<Real>(in.cash_amount),
                          1.0 /* notional */);

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        PricingSettings settings;
        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSDigitalAnalyticEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
