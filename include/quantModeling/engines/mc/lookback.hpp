#ifndef ENGINE_MC_LOOKBACK_HPP
#define ENGINE_MC_LOOKBACK_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/utils/greeks.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"

namespace quantModeling
{

    class BSEuroLookbackMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const LookbackOption &opt) override;

        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine: use BSEuroVanillaMCEngine for vanilla.");
        }

        void visit(const AsianOption &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine: use BSEuroAsianMCEngine for Asian.");
        }

        void visit(const BarrierOption &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine: use BSEuroBarrierMCEngine for barrier.");
        }

        void visit(const DigitalOption &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine: use BSDigitalAnalyticEngine for digital.");
        }

        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine does not support equity futures.");
        }

        void visit(const ZeroCouponBond &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine does not support bonds.");
        }

        void visit(const FixedRateBond &) override
        {
            throw UnsupportedInstrument("BSEuroLookbackMCEngine does not support bonds.");
        }

    private:
        static void validate(const LookbackOption &opt, int n_paths);
    };

} // namespace quantModeling

#endif
