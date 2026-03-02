#include "quantModeling/pricers/adapters/fx.hpp"

#include "quantModeling/engines/analytic/fx.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/instruments/fx/option.hpp"
#include "quantModeling/models/fx/garman_kohlhagen.hpp"
#include "quantModeling/pricers/pricer.hpp"

namespace quantModeling
{

    PricingResult price_fx_forward_analytic(const FXForwardInput &in)
    {
        auto model = std::make_shared<GarmanKohlhagenModel>(
            in.spot, in.rate_domestic, in.rate_foreign, in.vol);
        FXForward fwd(in.strike, in.maturity, in.notional);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        FXAnalyticEngine engine(ctx);
        return price(fwd, engine);
    }

    PricingResult price_fx_option_analytic(const FXOptionInput &in)
    {
        auto model = std::make_shared<GarmanKohlhagenModel>(
            in.spot, in.rate_domestic, in.rate_foreign, in.vol);
        FXOption opt(in.strike, in.maturity, in.is_call, in.notional);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        FXAnalyticEngine engine(ctx);
        return price(opt, engine);
    }

} // namespace quantModeling
