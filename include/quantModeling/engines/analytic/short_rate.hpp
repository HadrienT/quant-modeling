#ifndef ENGINE_ANALYTIC_SHORT_RATE_HPP
#define ENGINE_ANALYTIC_SHORT_RATE_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/bond_option.hpp"
#include "quantModeling/instruments/rates/capfloor.hpp"
#include "quantModeling/instruments/rates/caplet.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/rates/short_rate_model.hpp"

namespace quantModeling
{

    /**
     * @brief Analytic engine for instruments priced under short-rate models.
     *
     * Supports:
     *   - ZeroCouponBond   : P(0, T) from model
     *   - FixedRateBond    : sum of ZCB prices for coupons + principal
     *   - BondOption       : calls model.bond_option_price() (Vasicek/HW)
     *   - CapFloor         : decomposes into bond options
     *
     * Throws for equity instruments.
     */
    class ShortRateAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        // ── Rate instruments ─────────────────────────────────────────────────

        void visit(const ZeroCouponBond &bond) override;
        void visit(const FixedRateBond &bond) override;
        void visit(const BondOption &opt) override;
        void visit(const CapFloor &cf) override;
        void visit(const Caplet &cap) override;

        // ── Equity instruments — unsupported ─────────────────────────────────

        void visit(const VanillaOption &) override { unsupported("VanillaOption"); }
        void visit(const AsianOption &) override { unsupported("AsianOption"); }
        void visit(const BarrierOption &) override { unsupported("BarrierOption"); }
        void visit(const DigitalOption &) override { unsupported("DigitalOption"); }
        void visit(const EquityFuture &) override { unsupported("EquityFuture"); }
    };

} // namespace quantModeling

#endif
