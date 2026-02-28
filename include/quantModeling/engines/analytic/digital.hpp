#ifndef ENGINE_ANALYTIC_DIGITAL_HPP
#define ENGINE_ANALYTIC_DIGITAL_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/stats.hpp"

namespace quantModeling
{
    /**
     * Analytic pricing engine for European digital (binary) options.
     *
     * Supports:
     *   - Cash-or-Nothing call/put : pays cash_amount * e^(-rT) * N(±d2)
     *   - Asset-or-Nothing call/put: pays S * e^(-qT) * N(±d1)
     *
     * Greeks are not computed (they involve discontinuous-payoff sensitivities
     * that require special treatment).
     */
    class BSDigitalAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const DigitalOption &opt) override;

        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine: use BSEuroVanillaAnalyticEngine for vanilla.");
        }
        void visit(const AsianOption &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine does not support Asian options.");
        }
        void visit(const BarrierOption &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine does not support barrier options.");
        }
        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine does not support equity futures.");
        }
        void visit(const ZeroCouponBond &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine does not support bonds.");
        }
        void visit(const FixedRateBond &) override
        {
            throw UnsupportedInstrument("BSDigitalAnalyticEngine does not support bonds.");
        }

    private:
        static void validate(const DigitalOption &opt);
    };

} // namespace quantModeling

#endif
