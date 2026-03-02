#ifndef ENGINE_MC_DISPERSION_HPP
#define ENGINE_MC_DISPERSION_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/dispersion.hpp"

namespace quantModeling
{

    /**
     * @brief MC engine for dispersion swap under multi-asset BS.
     *
     * Simulates correlated assets, computes realised variance of each
     * constituent and of the weighted index, then computes:
     *   payoff = N × [ Σ w_i σ²_i − σ²_index − K_spread ] × df
     */
    class DispersionMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;
        void visit(const DispersionSwap &ds) override;

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
