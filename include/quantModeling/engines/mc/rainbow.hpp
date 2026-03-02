#ifndef ENGINE_MC_RAINBOW_HPP
#define ENGINE_MC_RAINBOW_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/rainbow.hpp"

namespace quantModeling
{

    /**
     * @brief MC engine for worst-of and best-of options under multi-asset BS.
     *
     * Simulates correlated terminal prices via Cholesky-decomposed GBM,
     * computes each asset's performance S_i(T)/S_i(0), then takes the
     * min (worst-of) or max (best-of) and applies the call/put payoff.
     *
     * Requires a MultiAssetBSModel with n_assets ≥ 2.
     */
    class RainbowMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const WorstOfOption &opt) override;
        void visit(const BestOfOption &opt) override;

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
