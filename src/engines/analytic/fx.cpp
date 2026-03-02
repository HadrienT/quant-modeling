#include "quantModeling/engines/analytic/fx.hpp"

#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    // ─── FX Forward ──────────────────────────────────────────────────────────

    void FXAnalyticEngine::visit(const FXForward &fwd)
    {
        const auto &m = require_model<ILocalVolModel>("FXAnalyticEngine");

        if (fwd.maturity <= 0.0)
            throw InvalidInput("FXForward: maturity must be > 0");
        if (fwd.strike <= 0.0)
            throw InvalidInput("FXForward: strike must be > 0");

        const Real S0 = m.spot0();    // spot FX rate
        const Real r_d = m.rate_r();  // domestic rate
        const Real r_f = m.yield_q(); // foreign rate
        const Real T = fwd.maturity;

        // F(0,T) = S0 × exp((r_d − r_f) × T)
        const Real F0 = S0 * std::exp((r_d - r_f) * T);
        const Real df = m.discount_curve().discount(T);

        PricingResult out;
        out.npv = fwd.notional * (F0 - fwd.strike) * df;
        out.diagnostics = "FXAnalyticEngine:Forward (F0=" + std::to_string(F0) + ")";
        res_ = out;
    }

    // ─── FX Option (Garman-Kohlhagen) ────────────────────────────────────────

    void FXAnalyticEngine::visit(const FXOption &opt)
    {
        const auto &m = require_model<ILocalVolModel>("FXAnalyticEngine");

        if (opt.maturity <= 0.0)
            throw InvalidInput("FXOption: maturity must be > 0");
        if (opt.strike <= 0.0)
            throw InvalidInput("FXOption: strike must be > 0");

        const Real S0 = m.spot0();
        const Real r_d = m.rate_r();
        const Real r_f = m.yield_q();
        const Real sigma = m.vol_sigma();
        const Real T = opt.maturity;
        const Real K = opt.strike;

        const Real sqrt_T = std::sqrt(T);
        const Real d1 = (std::log(S0 / K) + (r_d - r_f + 0.5 * sigma * sigma) * T) /
                        (sigma * sqrt_T);
        const Real d2 = d1 - sigma * sqrt_T;

        const Real df_d = m.discount_curve().discount(T);
        const Real df_f = std::exp(-r_f * T);

        PricingResult out;
        if (opt.is_call)
        {
            out.npv = opt.notional * (S0 * df_f * norm_cdf(d1) - K * df_d * norm_cdf(d2));
        }
        else
        {
            out.npv = opt.notional * (K * df_d * norm_cdf(-d2) - S0 * df_f * norm_cdf(-d1));
        }

        // Greeks
        out.greeks.delta = opt.notional * df_f * (opt.is_call ? norm_cdf(d1) : norm_cdf(d1) - 1.0);
        out.greeks.gamma = opt.notional * df_f * norm_pdf(d1) / (S0 * sigma * sqrt_T);
        out.greeks.vega = opt.notional * S0 * df_f * norm_pdf(d1) * sqrt_T;
        out.greeks.theta = opt.notional * (-(S0 * df_f * norm_pdf(d1) * sigma) / (2.0 * sqrt_T) +
                                           (opt.is_call
                                                ? (r_f * S0 * df_f * norm_cdf(d1) - r_d * K * df_d * norm_cdf(d2))
                                                : (-r_f * S0 * df_f * norm_cdf(-d1) + r_d * K * df_d * norm_cdf(-d2))));
        out.greeks.rho = opt.notional * K * T * df_d *
                         (opt.is_call ? norm_cdf(d2) : -norm_cdf(-d2));

        out.diagnostics = "FXAnalyticEngine:GarmanKohlhagen (d1=" +
                          std::to_string(d1) + ", d2=" + std::to_string(d2) + ")";
        res_ = out;
    }

    // ─── rejections ──────────────────────────────────────────────────────────

    void FXAnalyticEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void FXAnalyticEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void FXAnalyticEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void FXAnalyticEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void FXAnalyticEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void FXAnalyticEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void FXAnalyticEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
