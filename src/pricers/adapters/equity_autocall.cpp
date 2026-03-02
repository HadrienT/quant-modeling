#include "quantModeling/pricers/adapters/equity_autocall.hpp"

#include "quantModeling/engines/mc/autocall.hpp"
#include "quantModeling/instruments/equity/autocall.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_autocall_bs_mc(const AutocallBSInput &in)
    {
        // ── Build model ───────────────────────────────────────────────
        auto model = std::make_shared<BlackScholesModel>(
            in.spot, in.rate, in.dividend, in.vol);

        // ── Build instrument ──────────────────────────────────────────
        AutocallNote note(
            in.observation_dates,
            in.autocall_barrier,
            in.coupon_barrier,
            in.put_barrier,
            in.coupon_rate,
            in.notional,
            in.memory_coupon,
            in.ki_continuous);

        // ── Pricing context ───────────────────────────────────────────
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;

        MarketView market = {};
        PricingContext ctx{market, settings, model};

        BSAutocallMCEngine engine(ctx);
        return price(note, engine);
    }

} // namespace quantModeling
