#ifndef ENGINE_MC_VARIANCE_SWAP_HPP
#define ENGINE_MC_VARIANCE_SWAP_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/variance_swap.hpp"

namespace quantModeling
{

    /**
     * @brief MC engine for variance swap and volatility swap.
     *
     * Simulates GBM paths, computes realised variance from discrete log-returns,
     * then pays N × (σ²_realised − K_var) discounted.
     * Also prices VolatilitySwap: N × (σ_realised − K_vol) discounted.
     */
    class VolSwapMCEngine final : public EngineBase
    {
      public:
        using EngineBase::EngineBase;
        void visit(const VarianceSwap &vs) override;
        void visit(const VolatilitySwap &vs) override;
    };

} // namespace quantModeling

#endif
