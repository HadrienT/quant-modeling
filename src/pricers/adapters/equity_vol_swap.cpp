#include "quantModeling/pricers/adapters/equity_vol_swap.hpp"

#include "quantModeling/engines/analytic/variance_swap.hpp"
#include "quantModeling/instruments/equity/variance_swap.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/pricer.hpp"

namespace quantModeling
{

    PricingResult price_variance_swap_bs_analytic(const VarianceSwapBSInput &in)
    {
        auto model = std::make_shared<BlackScholesModel>(in.spot, in.rate, in.dividend, in.vol);
        VarianceSwap vs(in.maturity, in.strike_var, in.notional, in.observation_dates);
        PricingContext ctx{MarketView{}, PricingSettings{}, model};
        VarianceSwapAnalyticEngine engine(ctx);
        return price(vs, engine);
    }

} // namespace quantModeling
