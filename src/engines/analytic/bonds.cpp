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

        // Bond analytics: ZCB has a single cash flow at maturity
        // Macaulay Duration = T (for a ZCB)
        // Modified Duration = MacDur / (1 + r) — industry convention (annual compounding for ZCB)
        // Convexity = T × (T + 1) / (1 + r)² — discrete convexity for a single cash flow
        // DV01 = ModDur × Price × 0.0001
        const Real price = out.npv;
        const Real mac_dur = T;
        const Real mod_dur = mac_dur / (1.0 + r);
        const Real cvx = T * (T + 1.0) / ((1.0 + r) * (1.0 + r));
        const Real dv01 = mod_dur * price * 0.0001;

        out.bond_analytics.macaulay_duration = mac_dur;
        out.bond_analytics.modified_duration = mod_dur;
        out.bond_analytics.convexity = cvx;
        out.bond_analytics.dv01 = dv01;

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
        Real mac_dur_sum = 0.0; // Σ t_i × CF_i × df(t_i)

        for (int i = 1; i <= n; ++i)
        {
            const Real t = dt * static_cast<Real>(i);
            const Real df = discount(t);
            const Real pv_cf = coupon * df;
            pv_coupons += pv_cf;
            mac_dur_sum += t * pv_cf;
        }

        const Real pv_principal = bond.notional * discount(T);

        // Include principal in duration sum
        mac_dur_sum += T * pv_principal;

        PricingResult out;
        const Real price = pv_coupons + pv_principal;
        out.npv = price;
        out.diagnostics = "Flat-rate analytic fixed-rate bond";

        // Bond analytics
        // Macaulay Duration = Σ(t_i × CF_i × df(t_i)) / P
        // Modified Duration = MacDur / (1 + r / freq)  — industry convention
        // Convexity = Σ(t_i × (t_i + 1/freq) × CF_i × df(t_i)) / (P × (1 + r/freq)²)
        // DV01 = ModDur × Price × 0.0001
        const Real mac_dur = (price > 0.0) ? mac_dur_sum / price : 0.0;
        const Real ry = r / static_cast<Real>(freq);
        const Real mod_dur = mac_dur / (1.0 + ry);
        // Discrete convexity: Σ t_i(t_i + 1/freq) × PV_i / (P × (1+r/freq)²)
        const Real inv_freq = 1.0 / static_cast<Real>(freq);
        Real cvx_discrete_sum = 0.0;
        for (int i = 1; i <= n; ++i)
        {
            const Real t = dt * static_cast<Real>(i);
            const Real df_t = discount(t);
            cvx_discrete_sum += t * (t + inv_freq) * coupon * df_t;
        }
        cvx_discrete_sum += T * (T + inv_freq) * bond.notional * discount(T);
        const Real denom = (1.0 + ry) * (1.0 + ry);
        const Real cvx = (price > 0.0) ? cvx_discrete_sum / (price * denom) : 0.0;
        const Real dv01 = mod_dur * price * 0.0001;

        out.bond_analytics.macaulay_duration = mac_dur;
        out.bond_analytics.modified_duration = mod_dur;
        out.bond_analytics.convexity = cvx;
        out.bond_analytics.dv01 = dv01;

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
