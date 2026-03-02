#ifndef ENGINE_ANALYTIC_FX_HPP
#define ENGINE_ANALYTIC_FX_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/instruments/fx/option.hpp"

namespace quantModeling
{

    /**
     * @brief Analytic pricing engine for FX forwards and options.
     *
     * FX forward:  PV = N × [ S0 exp(−r_f T) − K exp(−r_d T) ]
     * FX option:   Garman-Kohlhagen = BS with q → r_f
     *
     * Requires a model providing spot0(), rate_r() (domestic), yield_q() (foreign),
     * and vol_sigma().  GarmanKohlhagenModel or BlackScholesModel both work.
     */
    class FXAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;
        void visit(const FXForward &fwd) override;
        void visit(const FXOption &opt) override;

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
