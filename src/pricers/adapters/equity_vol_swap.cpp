#include "quantModeling/pricers/adapters/equity_vol_swap.hpp"

#include "quantModeling/engines/analytic/variance_swap.hpp"
#include "quantModeling/engines/mc/variance_swap.hpp"
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

    PricingResult price_variance_swap_bs_mc(const VarianceSwapBSInput &in)
    {
        auto model = std::make_shared<BlackScholesModel>(in.spot, in.rate, in.dividend, in.vol);
        VarianceSwap vs(in.maturity, in.strike_var, in.notional, in.observation_dates);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        PricingContext ctx{MarketView{}, settings, model};
        VolSwapMCEngine engine(ctx);
        return price(vs, engine);
    }

    PricingResult price_volatility_swap_bs_mc(const VolatilitySwapBSInput &in)
    {
        auto model = std::make_shared<BlackScholesModel>(in.spot, in.rate, in.dividend, in.vol);
        VolatilitySwap vs(in.maturity, in.strike_vol, in.notional, in.observation_dates);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        PricingContext ctx{MarketView{}, settings, model};
        VolSwapMCEngine engine(ctx);
        return price(vs, engine);
    }

} // namespace quantModeling
