#ifndef ENGINE_MC_AUTOCALL_HPP
#define ENGINE_MC_AUTOCALL_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/autocall.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"
#include "quantModeling/instruments/equity/basket.hpp"
#include "quantModeling/instruments/equity/digital.hpp"
#include "quantModeling/instruments/equity/lookback.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/equity/future.hpp"
#include "quantModeling/instruments/rates/fixed_rate_bond.hpp"
#include "quantModeling/instruments/rates/zero_coupon_bond.hpp"

namespace quantModeling
{

    /**
     * @brief Monte Carlo engine for Autocallable notes under Black-Scholes.
     *
     * Simulates the spot path at each observation date using GBM:
     *   S(T_i) = S0 × exp((r − q − 0.5σ²)T_i + σ√T_i Z_i)
     *
     * Path-correlated draws are used so that the autocall barrier check,
     * coupon barrier check, and knock-in put barrier check are all
     * consistent within each path.
     *
     * If ki_continuous is false, the put barrier is checked only at the
     * final observation date.  If true, it is checked at every step.
     */
    class BSAutocallMCEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const AutocallNote &note) override;

        /* ── reject all other instrument types ────────────────────────── */
        void visit(const VanillaOption &) override { unsupported("AutocallNote"); }
        void visit(const AsianOption &) override { unsupported("AutocallNote"); }
        void visit(const BarrierOption &) override { unsupported("AutocallNote"); }
        void visit(const DigitalOption &) override { unsupported("AutocallNote"); }
        void visit(const LookbackOption &) override { unsupported("AutocallNote"); }
        void visit(const BasketOption &) override { unsupported("AutocallNote"); }
        void visit(const EquityFuture &) override { unsupported("AutocallNote"); }
        void visit(const ZeroCouponBond &) override { unsupported("AutocallNote"); }
        void visit(const FixedRateBond &) override { unsupported("AutocallNote"); }
    };

} // namespace quantModeling

#endif
