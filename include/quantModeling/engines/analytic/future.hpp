#ifndef ENGINE_ANALYTIC_EQUITY_FUTURE_HPP
#define ENGINE_ANALYTIC_EQUITY_FUTURE_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"

namespace quantModeling
{

    class BSEquityFutureAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const EquityFuture &fut) override;
        void visit(const VanillaOption &) override;
        void visit(const AsianOption &) override;
        void visit(const ZeroCouponBond &) override;
        void visit(const FixedRateBond &) override;

    private:
        static void validate(const EquityFuture &fut);
    };

} // namespace quantModeling

#endif
