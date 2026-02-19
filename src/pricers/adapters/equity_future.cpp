#include "quantModeling/pricers/adapters/equity_future.hpp"

#include "quantModeling/engines/analytic/future.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_future_bs(const EquityFutureInput &in)
    {
        EquityFuture fut(in.strike, in.maturity, in.notional);

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            0.0);

        MarketView market = {};
        PricingSettings settings = {0, 0, true, 0, 0, 0};
        PricingContext ctx{market, settings, model};

        BSEquityFutureAnalyticEngine engine(ctx);
        return price(fut, engine);
    }

} // namespace quantModeling
