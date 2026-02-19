#include "quantModeling/engines/analytic/future.hpp"

#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"

#include <cmath>

namespace quantModeling
{

    void BSEquityFutureAnalyticEngine::visit(const EquityFuture &fut)
    {
        validate(fut);
        const auto &m = require_model<ILocalVolModel>("BSEquityFutureAnalyticEngine");

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real T = fut.maturity;
        const Real K = fut.strike;

        const Real F0 = S0 * std::exp((r - q) * T);
        const Real df = std::exp(-r * T);

        PricingResult out;
        out.npv = fut.notional * (F0 - K) * df;
        out.diagnostics = "Equity future analytic (cost-of-carry)";

        res_ = out;
    }

    void BSEquityFutureAnalyticEngine::visit(const VanillaOption &)
    {
        throw UnsupportedInstrument("BSEquityFutureAnalyticEngine does not support vanilla options.");
    }

    void BSEquityFutureAnalyticEngine::visit(const AsianOption &)
    {
        throw UnsupportedInstrument("BSEquityFutureAnalyticEngine does not support Asian options.");
    }

    void BSEquityFutureAnalyticEngine::visit(const ZeroCouponBond &)
    {
        throw UnsupportedInstrument("BSEquityFutureAnalyticEngine does not support bonds.");
    }

    void BSEquityFutureAnalyticEngine::visit(const FixedRateBond &)
    {
        throw UnsupportedInstrument("BSEquityFutureAnalyticEngine does not support bonds.");
    }

    void BSEquityFutureAnalyticEngine::validate(const EquityFuture &fut)
    {
        if (!(fut.maturity > 0.0))
            throw InvalidInput("EquityFuture maturity must be > 0");
        if (!(fut.notional != 0.0))
            throw InvalidInput("EquityFuture notional must be non-zero");
        if (!(fut.strike > 0.0))
            throw InvalidInput("EquityFuture strike must be > 0");
    }

} // namespace quantModeling
