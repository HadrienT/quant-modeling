#include "quantModeling/engines/analytic/bonds.hpp"

#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/rates/flat_rate.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    void FlatRateBondAnalyticEngine::visit(const ZeroCouponBond &bond)
    {
        validate(bond);
        const auto &m = require_model<IFlatRateModel>("FlatRateBondAnalyticEngine");

        const auto *curve = ctx_.market.discount.get();
        const Real r = m.rate();
        const Real T = bond.maturity;
        const Real df = curve ? curve->discount(T) : std::exp(-r * T);

        PricingResult out;
        out.npv = bond.notional * df;
        out.diagnostics = "Flat-rate analytic zero coupon bond";
        res_ = out;
    }

    void FlatRateBondAnalyticEngine::visit(const FixedRateBond &bond)
    {
        validate(bond);
        const auto &m = require_model<IFlatRateModel>("FlatRateBondAnalyticEngine");

        const auto *curve = ctx_.market.discount.get();
        const Real r = m.rate();
        const Real T = bond.maturity;
        const int freq = bond.coupon_frequency;
        const int n = std::max(1, static_cast<int>(std::round(T * static_cast<Real>(freq))));
        const Real dt = T / static_cast<Real>(n);
        const Real coupon = bond.notional * bond.coupon_rate * dt;

        auto discount = [&](Real t)
        {
            return curve ? curve->discount(t) : std::exp(-r * t);
        };

        Real pv_coupons = 0.0;
        for (int i = 1; i <= n; ++i)
        {
            const Real t = dt * static_cast<Real>(i);
            pv_coupons += coupon * discount(t);
        }

        const Real pv_principal = bond.notional * discount(T);

        PricingResult out;
        out.npv = pv_coupons + pv_principal;
        out.diagnostics = "Flat-rate analytic fixed-rate bond";
        res_ = out;
    }

    void FlatRateBondAnalyticEngine::validate(const ZeroCouponBond &bond)
    {
        if (!(bond.maturity > 0.0))
            throw InvalidInput("ZeroCouponBond maturity must be > 0");
        if (!(bond.notional != 0.0))
            throw InvalidInput("ZeroCouponBond notional must be non-zero");
    }

    void FlatRateBondAnalyticEngine::validate(const FixedRateBond &bond)
    {
        if (!(bond.maturity > 0.0))
            throw InvalidInput("FixedRateBond maturity must be > 0");
        if (!(bond.notional != 0.0))
            throw InvalidInput("FixedRateBond notional must be non-zero");
        if (!(bond.coupon_rate >= 0.0))
            throw InvalidInput("FixedRateBond coupon rate must be >= 0");
        if (bond.coupon_frequency < 1)
            throw InvalidInput("FixedRateBond coupon frequency must be >= 1");
    }

} // namespace quantModeling
