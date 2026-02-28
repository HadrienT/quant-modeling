#ifndef ENGINE_MC_BASKET_HPP
#define ENGINE_MC_BASKET_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/basket.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"
#include "quantModeling/utils/greeks.hpp"

namespace quantModeling
{

    /**
     * @brief Monte Carlo engine for European basket options under multi-asset Black-Scholes.
     *
     * Simulates correlated GBM terminal prices using a Cholesky factorisation of the
     * correlation matrix (supplied via MultiAssetBSModel).  Greeks are computed via:
     *   - Delta:  pathwise (sum of per-asset pathwise sensitivities)
     *   - Gamma:  FD central difference with common random numbers (parallel spot scale)
     *   - Vega:   FD central difference with CRN (all vols shifted simultaneously)
     *   - Rho:    FD central difference with CRN (rate shifted)
     *   - Theta:  FD central difference with CRN (maturity shifted)
     */
    class BSBasketMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const BasketOption &opt) override;

        /* ── throw for all irrelevant instrument types ─────────────────── */
        void visit(const VanillaOption &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine: use BSEuroVanillaMCEngine for vanilla.");
        }
        void visit(const AsianOption &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine: use BSEuroAsianMCEngine for Asian.");
        }
        void visit(const BarrierOption &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine: use BSEuroBarrierMCEngine for barrier.");
        }
        void visit(const DigitalOption &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine: use BSDigitalAnalyticEngine for digital.");
        }
        void visit(const LookbackOption &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine: use BSEuroLookbackMCEngine for lookback.");
        }
        void visit(const EquityFuture &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine does not support equity futures.");
        }
        void visit(const ZeroCouponBond &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine does not support bonds.");
        }
        void visit(const FixedRateBond &) override
        {
            throw UnsupportedInstrument("BSBasketMCEngine does not support bonds.");
        }

    private:
        static void validate(const BasketOption &opt, int n_assets, int n_paths);
    };

} // namespace quantModeling

#endif
