#include "quantModeling/pricers/adapters/rates_short_rate.hpp"

#include "quantModeling/engines/analytic/short_rate.hpp"
#include "quantModeling/engines/mc/short_rate.hpp"
#include "quantModeling/instruments/rates/bond_option.hpp"
#include "quantModeling/instruments/rates/capfloor.hpp"
#include "quantModeling/instruments/rates/caplet.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/rates/cir.hpp"
#include "quantModeling/models/rates/hull_white.hpp"
#include "quantModeling/models/rates/vasicek.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>
#include <stdexcept>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────────
    //  Helper to build the correct IShortRateModel from a model_type string
    // ─────────────────────────────────────────────────────────────────────────

    namespace
    {

        std::shared_ptr<IShortRateModel> make_short_rate_model(
            const std::string &model_type,
            Real a, Real b, Real sigma, Real r0)
        {
            if (model_type == "vasicek")
                return std::make_shared<VasicekModel>(a, b, sigma, r0);
            if (model_type == "cir")
                return std::make_shared<CIRModel>(a, b, sigma, r0);
            if (model_type == "hull_white")
                return std::make_shared<HullWhiteModel>(a, sigma, r0, /*theta=*/a * b);
            throw InvalidInput("Unknown short-rate model type: " + model_type);
        }

        PricingContext make_context(std::shared_ptr<const IModel> model,
                                    const PricingSettings &settings = {})
        {
            return PricingContext{MarketView{}, settings, std::move(model)};
        }

    } // anonymous namespace

    // ── ZCB ──────────────────────────────────────────────────────────────────

    PricingResult price_zcb_short_rate(const ShortRateZCBInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        ZeroCouponBond bond(in.maturity, in.notional);
        auto ctx = make_context(model);
        ShortRateAnalyticEngine engine(ctx);
        return price(bond, engine);
    }

    // ── FixedRateBond ────────────────────────────────────────────────────────

    PricingResult price_fixed_bond_short_rate(const ShortRateBondInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        FixedRateBond bond(in.coupon_rate, in.maturity, in.coupon_frequency, in.notional);
        auto ctx = make_context(model);
        ShortRateAnalyticEngine engine(ctx);
        return price(bond, engine);
    }

    // ── BondOption — Analytic ────────────────────────────────────────────────

    PricingResult price_bond_option_short_rate_analytic(const ShortRateBondOptionInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        BondOption opt(in.option_maturity, in.bond_maturity, in.strike, in.is_call, in.notional);
        auto ctx = make_context(model);
        ShortRateAnalyticEngine engine(ctx);
        return price(opt, engine);
    }

    // ── BondOption — MC ──────────────────────────────────────────────────────

    PricingResult price_bond_option_short_rate_mc(const ShortRateBondOptionInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        BondOption opt(in.option_maturity, in.bond_maturity, in.strike, in.is_call, in.notional);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        auto ctx = make_context(model, settings);
        ShortRateMCEngine engine(ctx);
        return price(opt, engine);
    }

    // ── CapFloor — Analytic ──────────────────────────────────────────────────

    PricingResult price_capfloor_short_rate_analytic(const ShortRateCapFloorInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        CapFloor cf(in.schedule, in.strike, in.is_cap, in.notional);
        auto ctx = make_context(model);
        ShortRateAnalyticEngine engine(ctx);
        return price(cf, engine);
    }

    // ── CapFloor — MC ────────────────────────────────────────────────────────

    PricingResult price_capfloor_short_rate_mc(const ShortRateCapFloorInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        CapFloor cf(in.schedule, in.strike, in.is_cap, in.notional);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        auto ctx = make_context(model, settings);
        ShortRateMCEngine engine(ctx);
        return price(cf, engine);
    }

    // ── Caplet — Analytic ────────────────────────────────────────────────────

    PricingResult price_caplet_short_rate_analytic(const ShortRateCapletInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        Caplet cap(in.start, in.end, in.strike, in.is_cap, in.notional);
        auto ctx = make_context(model);
        ShortRateAnalyticEngine engine(ctx);
        return price(cap, engine);
    }

    // ── Caplet — MC ──────────────────────────────────────────────────────────

    PricingResult price_caplet_short_rate_mc(const ShortRateCapletInput &in)
    {
        auto model = make_short_rate_model(in.model_type, in.a, in.b, in.sigma, in.r0);
        Caplet cap(in.start, in.end, in.strike, in.is_cap, in.notional);
        PricingSettings settings;
        settings.mc_paths = in.n_paths;
        settings.mc_seed = in.seed;
        auto ctx = make_context(model, settings);
        ShortRateMCEngine engine(ctx);
        return price(cap, engine);
    }

} // namespace quantModeling
