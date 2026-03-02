#ifndef ENGINE_ANALYTIC_VARIANCE_SWAP_HPP
#define ENGINE_ANALYTIC_VARIANCE_SWAP_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/variance_swap.hpp"

namespace quantModeling
{

    /**
     * @brief Analytic variance swap engine under BS / ILocalVolModel.
     *
     * Under GBM with constant vol σ, the fair variance is σ².
     * Price = N × (σ² − K_var) × exp(−r T).
     */
    class VarianceSwapAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;
        void visit(const VarianceSwap &vs) override;

        // reject everything else
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
