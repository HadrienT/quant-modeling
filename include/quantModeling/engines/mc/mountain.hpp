#ifndef ENGINE_MC_MOUNTAIN_HPP
#define ENGINE_MC_MOUNTAIN_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/autocall.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/basket.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/mountain.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"

namespace quantModeling
{

    /**
     * @brief Monte Carlo engine for Himalaya (Mountain) options
     *        under multi-asset Black-Scholes.
     *
     * At each observation date the best-performing asset is locked in and
     * removed from the basket.  The payoff is the call/put on the average
     * of locked-in returns.
     *
     * Requires a MultiAssetBSModel with n_assets == observation_dates.size().
     */
    class BSMountainMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const MountainOption &opt) override;

        /* ── reject all other instrument types ────────────────────────── */
        void visit(const VanillaOption &) override { unsupported("MountainOption"); }
        void visit(const AsianOption &) override { unsupported("MountainOption"); }
        void visit(const BarrierOption &) override { unsupported("MountainOption"); }
        void visit(const DigitalOption &) override { unsupported("MountainOption"); }
        void visit(const LookbackOption &) override { unsupported("MountainOption"); }
        void visit(const BasketOption &) override { unsupported("MountainOption"); }
        void visit(const EquityFuture &) override { unsupported("MountainOption"); }
        void visit(const ZeroCouponBond &) override { unsupported("MountainOption"); }
        void visit(const FixedRateBond &) override { unsupported("MountainOption"); }
    };

} // namespace quantModeling

#endif
