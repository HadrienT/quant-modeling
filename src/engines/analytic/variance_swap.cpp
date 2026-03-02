#include "quantModeling/engines/analytic/variance_swap.hpp"

#include "quantModeling/models/equity/local_vol_model.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    void VarianceSwapAnalyticEngine::visit(const VarianceSwap &vs)
    {
        const auto &m = require_model<ILocalVolModel>("VarianceSwapAnalyticEngine");

        if (vs.maturity <= 0.0)
            throw InvalidInput("VarianceSwap: maturity must be > 0");
        if (vs.notional == 0.0)
            throw InvalidInput("VarianceSwap: notional must be non-zero");

        const Real sigma = m.vol_sigma();
        const Real r = m.rate_r();
        const Real T = vs.maturity;

        // Under BS (constant vol), fair variance = σ²
        const Real realised_var = sigma * sigma;
        const Real df = m.discount_curve().discount(T);

        PricingResult out;
        out.npv = vs.notional * (realised_var - vs.strike_var) * df;
        out.diagnostics = "VarianceSwapAnalyticEngine (BS, fair_var=" +
                          std::to_string(realised_var) + ")";
        res_ = out;
    }

    void VarianceSwapAnalyticEngine::visit(const VanillaOption &) { unsupported("VanillaOption"); }
    void VarianceSwapAnalyticEngine::visit(const AsianOption &) { unsupported("AsianOption"); }
    void VarianceSwapAnalyticEngine::visit(const BarrierOption &) { unsupported("BarrierOption"); }
    void VarianceSwapAnalyticEngine::visit(const DigitalOption &) { unsupported("DigitalOption"); }
    void VarianceSwapAnalyticEngine::visit(const EquityFuture &) { unsupported("EquityFuture"); }
    void VarianceSwapAnalyticEngine::visit(const ZeroCouponBond &) { unsupported("ZeroCouponBond"); }
    void VarianceSwapAnalyticEngine::visit(const FixedRateBond &) { unsupported("FixedRateBond"); }

} // namespace quantModeling
