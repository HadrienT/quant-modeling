#include "quantModeling/pricers/adapters/bonds.hpp"

#include "quantModeling/engines/analytic/bonds.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/rates/flat_rate.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_zero_coupon_bond_flat(const ZeroCouponBondInput &in)
    {
        ZeroCouponBond bond(in.maturity, in.notional);

        auto model = std::make_shared<FlatRateModel>(static_cast<Real>(in.rate));
        std::shared_ptr<const DiscountCurve> curve;
        if (!in.discount_times.empty() || !in.discount_factors.empty())
        {
            curve = std::make_shared<DiscountCurve>(in.discount_times, in.discount_factors);
        }
        else
        {
            curve = std::make_shared<DiscountCurve>(static_cast<Real>(in.rate));
        }

        MarketView market = {};
        market.discount = curve;
        PricingSettings settings = {0, 0, true, 0, 0, 0};
        PricingContext ctx{market, settings, model};

        FlatRateBondAnalyticEngine engine(ctx);
        return price(bond, engine);
    }

    PricingResult price_fixed_rate_bond_flat(const FixedRateBondInput &in)
    {
        FixedRateBond bond(in.coupon_rate, in.maturity, in.coupon_frequency, in.notional);

        auto model = std::make_shared<FlatRateModel>(static_cast<Real>(in.rate));
        std::shared_ptr<const DiscountCurve> curve;
        if (!in.discount_times.empty() || !in.discount_factors.empty())
        {
            curve = std::make_shared<DiscountCurve>(in.discount_times, in.discount_factors);
        }
        else
        {
            curve = std::make_shared<DiscountCurve>(static_cast<Real>(in.rate));
        }

        MarketView market = {};
        market.discount = curve;
        PricingSettings settings = {0, 0, true, 0, 0, 0};
        PricingContext ctx{market, settings, model};

        FlatRateBondAnalyticEngine engine(ctx);
        return price(bond, engine);
    }

} // namespace quantModeling
