#include "quantModeling/engines/analytic/commodity.hpp"

#include "quantModeling/models/commodity/commodity_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    // ─── Commodity Forward ───────────────────────────────────────────────────

    void CommodityAnalyticEngine::visit(const CommodityForward &fwd)
    {
        const auto &m = require_model<ICommodityModel>("CommodityAnalyticEngine");

        if (fwd.maturity <= 0.0)
            throw InvalidInput("CommodityForward: maturity must be > 0");
        if (fwd.strike <= 0.0)
            throw InvalidInput("CommodityForward: strike must be > 0");

        const Real T = fwd.maturity;
        const Real F = m.forward(T);
        const Real df = m.discount_curve().discount(T);

        PricingResult out;
        out.npv = fwd.notional * (F - fwd.strike) * df;
        out.diagnostics = "CommodityAnalyticEngine:Forward (F=" +
                          std::to_string(F) + ")";
        res_ = out;
    }

    // ─── Commodity Option (Black '76) ────────────────────────────────────────

    void CommodityAnalyticEngine::visit(const CommodityOption &opt)
    {
        const auto &m = require_model<ICommodityModel>("CommodityAnalyticEngine");

        if (opt.maturity <= 0.0)
            throw InvalidInput("CommodityOption: maturity must be > 0");
        if (opt.strike <= 0.0)
            throw InvalidInput("CommodityOption: strike must be > 0");

        const Real T = opt.maturity;
        const Real K = opt.strike;
        const Real F = m.forward(T);
        const Real sigma = m.vol_sigma();
        const Real r = m.rate();
        const Real df = m.discount_curve().discount(T);

        const Real sqrt_T = std::sqrt(T);
        const Real d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * sqrt_T);
        const Real d2 = d1 - sigma * sqrt_T;

        PricingResult out;
        if (opt.is_call)
        {
            out.npv = opt.notional * df * (F * norm_cdf(d1) - K * norm_cdf(d2));
        }
        else
        {
            out.npv = opt.notional * df * (K * norm_cdf(-d2) - F * norm_cdf(-d1));
        }

        // Greeks (Black '76 sensitivities)
        out.greeks.delta = opt.notional * df * (opt.is_call ? norm_cdf(d1) : norm_cdf(d1) - 1.0);
        out.greeks.gamma = opt.notional * df * norm_pdf(d1) / (F * sigma * sqrt_T);
        out.greeks.vega = opt.notional * df * F * norm_pdf(d1) * sqrt_T;
        out.greeks.theta = opt.notional * (-df * F * norm_pdf(d1) * sigma / (2.0 * sqrt_T) -
                                           r * out.npv / opt.notional);
        out.greeks.rho = -T * out.npv;

        out.diagnostics = "CommodityAnalyticEngine:Black76 (F=" +
                          std::to_string(F) + ", d1=" + std::to_string(d1) +
                          ", d2=" + std::to_string(d2) + ")";
        res_ = out;
    }

    // ─── rejections ──────────────────────────────────────────────────────────

    void CommodityAnalyticEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void CommodityAnalyticEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void CommodityAnalyticEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void CommodityAnalyticEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void CommodityAnalyticEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void CommodityAnalyticEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void CommodityAnalyticEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
