#ifndef ENGINE_MC_SHORT_RATE_HPP
#define ENGINE_MC_SHORT_RATE_HPP

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
     * @brief Monte-Carlo engine for instruments priced under short-rate models.
     *
     * Simulates rate paths via the model's euler_step() method.
     * Works for Vasicek, CIR, Hull-White, and any future IShortRateModel.
     *
     * Supports:
     *   - ZeroCouponBond   : E[exp(−∫₀ᵀ r(s) ds)]
     *   - FixedRateBond    : sum of discounted coupon/principal cash flows
     *   - BondOption       : simulate to T_option, use analytic P(T,S,r(T))
     *   - CapFloor         : decompose into caplets, sum MC bond-option prices
     *
     * Throws for equity instruments.
     */
    class ShortRateMCEngine final : public EngineBase
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
