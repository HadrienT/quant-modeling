#include "quantModeling/engines/analytic/short_rate.hpp"

#include "quantModeling/models/rates/short_rate_model.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    // ── ZeroCouponBond ───────────────────────────────────────────────────────

    void ShortRateAnalyticEngine::visit(const ZeroCouponBond &bond)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateAnalyticEngine");

        if (bond.maturity <= 0.0)
            throw InvalidInput("ShortRateAnalyticEngine: ZCB maturity must be > 0");

        const Real P = m.zcb_price(bond.maturity);

        PricingResult out;
        out.npv = bond.notional * P;
        out.diagnostics = "ShortRateAnalyticEngine ZCB (" + m.model_name() + ")";

        // Bond analytics
        const Real r = m.r0();
        const Real T = bond.maturity;
        out.bond_analytics.macaulay_duration = T;
        out.bond_analytics.modified_duration = T / (1.0 + r);
        out.bond_analytics.convexity = T * (T + 1.0) / ((1.0 + r) * (1.0 + r));
        out.bond_analytics.dv01 = (T / (1.0 + r)) * out.npv * 0.0001;

        res_ = out;
    }

    // ── FixedRateBond ────────────────────────────────────────────────────────

    void ShortRateAnalyticEngine::visit(const FixedRateBond &bond)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateAnalyticEngine");

        if (bond.maturity <= 0.0)
            throw InvalidInput("ShortRateAnalyticEngine: FixedRateBond maturity must be > 0");

        const int freq = bond.coupon_frequency;
        const int n = std::max(1, static_cast<int>(std::round(
                                      bond.maturity * static_cast<Real>(freq))));
        const Real dt = bond.maturity / static_cast<Real>(n);
        const Real coupon = bond.notional * bond.coupon_rate * dt;

        Real pv_coupons = 0.0;
        Real mac_dur_sum = 0.0;

        for (int i = 1; i <= n; ++i)
        {
            const Real t = dt * static_cast<Real>(i);
            const Real df = m.zcb_price(t);
            const Real pv_cf = coupon * df;
            pv_coupons += pv_cf;
            mac_dur_sum += t * pv_cf;
        }

        const Real pv_principal = bond.notional * m.zcb_price(bond.maturity);
        mac_dur_sum += bond.maturity * pv_principal;

        PricingResult out;
        const Real price = pv_coupons + pv_principal;
        out.npv = price;
        out.diagnostics = "ShortRateAnalyticEngine FixedRateBond (" + m.model_name() + ")";

        // Bond analytics
        const Real r = m.r0();
        const Real mac_dur = (price > 0.0) ? mac_dur_sum / price : 0.0;
        const Real ry = r / static_cast<Real>(freq);
        out.bond_analytics.macaulay_duration = mac_dur;
        out.bond_analytics.modified_duration = mac_dur / (1.0 + ry);
        out.bond_analytics.dv01 = (mac_dur / (1.0 + ry)) * price * 0.0001;

        // Convexity
        const Real inv_freq = 1.0 / static_cast<Real>(freq);
        Real cvx_sum = 0.0;
        for (int i = 1; i <= n; ++i)
        {
            const Real t = dt * static_cast<Real>(i);
            const Real df = m.zcb_price(t);
            cvx_sum += t * (t + inv_freq) * coupon * df;
        }
        cvx_sum += bond.maturity * (bond.maturity + inv_freq) * bond.notional *
                   m.zcb_price(bond.maturity);
        const Real denom = (1.0 + ry) * (1.0 + ry);
        out.bond_analytics.convexity = (price > 0.0) ? cvx_sum / (price * denom) : 0.0;

        res_ = out;
    }

    // ── BondOption ───────────────────────────────────────────────────────────

    void ShortRateAnalyticEngine::visit(const BondOption &opt)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateAnalyticEngine");

        // Delegates to model's analytic formula (Vasicek, Hull-White).
        // CIRModel::bond_option_price() throws UnsupportedInstrument — use MC.
        const Real unit_price = m.bond_option_price(
            opt.is_call, opt.strike, opt.option_maturity, opt.bond_maturity);

        PricingResult out;
        out.npv = opt.notional * unit_price;
        out.diagnostics = "ShortRateAnalyticEngine BondOption (" + m.model_name() + ")";
        res_ = out;
    }

    // ── CapFloor ─────────────────────────────────────────────────────────────
    //
    //  Cap  = Σ_i (1 + K δ_i) · Put(P(·,T_{i+1}), K_p, T_i)
    //  Floor= Σ_i (1 + K δ_i) · Call(P(·,T_{i+1}), K_p, T_i)
    //  where K_p = 1 / (1 + K δ_i),  δ_i = T_{i+1} − T_i

    void ShortRateAnalyticEngine::visit(const CapFloor &cf)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateAnalyticEngine");

        if (cf.schedule.size() < 2)
            throw InvalidInput("ShortRateAnalyticEngine: CapFloor schedule needs ≥ 2 dates");

        Real total = 0.0;
        const auto n = cf.schedule.size() - 1;

        for (std::size_t i = 0; i < n; ++i)
        {
            const Real Ti = cf.schedule[i];
            const Real Ti1 = cf.schedule[i + 1];
            const Real delta = Ti1 - Ti;
            if (delta <= 0.0)
                throw InvalidInput("ShortRateAnalyticEngine: schedule dates must be increasing");

            // Skip caplets where option expiry ≤ 0 (already expired)
            if (Ti <= 0.0)
                continue;

            const Real K_delta = cf.strike * delta;
            const Real factor = 1.0 + K_delta;
            const Real K_p = 1.0 / factor;

            // Caplet = factor × Put; Floorlet = factor × Call
            const bool bond_opt_is_call = !cf.is_cap;
            const Real bo = m.bond_option_price(bond_opt_is_call, K_p, Ti, Ti1);
            total += factor * bo;
        }

        PricingResult out;
        out.npv = cf.notional * total;
        out.diagnostics = "ShortRateAnalyticEngine CapFloor (" + m.model_name() + ")";
        res_ = out;
    }

    // ── Caplet ───────────────────────────────────────────────────────────────
    //
    //  Standalone caplet/floorlet:
    //    Caplet  = (1 + Kδ) × Put(K_p, T_start, T_end)
    //    Floorlet= (1 + Kδ) × Call(K_p, T_start, T_end)
    //  where K_p = 1 / (1 + Kδ),  δ = T_end − T_start

    void ShortRateAnalyticEngine::visit(const Caplet &cap)
    {
        const auto &m = require_model<IShortRateModel>("ShortRateAnalyticEngine");

        if (cap.start <= 0.0)
            throw InvalidInput("ShortRateAnalyticEngine: Caplet start must be > 0");
        if (cap.end <= cap.start)
            throw InvalidInput("ShortRateAnalyticEngine: Caplet end must be > start");

        const Real delta = cap.end - cap.start;
        const Real K_delta = cap.strike * delta;
        const Real factor = 1.0 + K_delta;
        const Real K_p = 1.0 / factor;

        const bool bond_opt_is_call = !cap.is_cap;
        const Real bo = m.bond_option_price(bond_opt_is_call, K_p, cap.start, cap.end);

        PricingResult out;
        out.npv = cap.notional * factor * bo;
        out.diagnostics = "ShortRateAnalyticEngine Caplet (" + m.model_name() + ")";
        res_ = out;
    }

} // namespace quantModeling
