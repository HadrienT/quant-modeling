#ifndef ENGINE_ANALYTIC_BONDS_HPP
#define ENGINE_ANALYTIC_BONDS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/rates/flat_rate.hpp"

namespace quantModeling
{

    class FlatRateBondAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const ZeroCouponBond &bond) override;
        void visit(const FixedRateBond &bond) override;

        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("FlatRateBondAnalyticEngine does not support vanilla options.");
        }
        void visit(const AsianOption &) override
        {
            throw UnsupportedInstrument("FlatRateBondAnalyticEngine does not support Asian options.");
        }
        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("FlatRateBondAnalyticEngine does not support equity futures.");
        }

    private:
        static void validate(const ZeroCouponBond &bond);
        static void validate(const FixedRateBond &bond);
    };

} // namespace quantModeling

#endif
