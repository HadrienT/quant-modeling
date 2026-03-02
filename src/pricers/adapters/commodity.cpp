#include "quantModeling/pricers/adapters/commodity.hpp"

#include "quantModeling/engines/analytic/commodity.hpp"
#include "quantModeling/instruments/commodity/forward.hpp"
#include "quantModeling/instruments/commodity/option.hpp"
#include "quantModeling/models/commodity/commodity_model.hpp"
#include "quantModeling/pricers/pricer.hpp"

namespace quantModeling
{

    PricingResult price_commodity_forward_analytic(const CommodityForwardInput &in)
    {
        auto model = std::make_shared<CommodityBlackModel>(
            in.spot, in.rate, in.storage_cost, in.convenience_yield, in.vol);
        CommodityForward fwd(in.strike, in.maturity, in.notional);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        CommodityAnalyticEngine engine(ctx);
        return price(fwd, engine);
    }

    PricingResult price_commodity_option_analytic(const CommodityOptionInput &in)
    {
        auto model = std::make_shared<CommodityBlackModel>(
            in.spot, in.rate, in.storage_cost, in.convenience_yield, in.vol);
        CommodityOption opt(in.strike, in.maturity, in.is_call, in.notional);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        CommodityAnalyticEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
