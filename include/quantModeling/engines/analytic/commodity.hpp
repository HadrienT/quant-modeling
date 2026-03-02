#ifndef ENGINE_ANALYTIC_COMMODITY_HPP
#define ENGINE_ANALYTIC_COMMODITY_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/commodity/forward.hpp"
#include "quantModeling/instruments/commodity/option.hpp"

namespace quantModeling
{

    /**
     * @brief Analytic pricing engine for commodity forwards and options.
     *
     * Commodity forward:  PV = N × (F − K) × exp(−r T)
     *                     where F = S0 × exp((r + u − y) T)
     * Commodity option:   Black '76 on the forward price.
     *
     * Requires ICommodityModel.
     */
    class CommodityAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;
        void visit(const CommodityForward &fwd) override;
        void visit(const CommodityOption &opt) override;

        void visit(const VanillaOption &) override;
        void visit(const AsianOption &) override;
        void visit(const BarrierOption &) override;
        void visit(const DigitalOption &) override;
        void visit(const EquityFuture &) override;
        void visit(const ZeroCouponBond &) override;
        void visit(const FixedRateBond &) override;
    };

} // namespace quantModeling

#endif
