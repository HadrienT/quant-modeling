#ifndef INSTRUMENT_EQUITY_BARRIER_HPP
#define INSTRUMENT_EQUITY_BARRIER_HPP

#include "quantModeling/instruments/base.hpp"
#include <memory>

namespace quantModeling
{

    /**
     * Barrier type: encodes direction (up/down) and knock flavour (in/out).
     *
     *  UpAndIn   — activated only when spot rises ABOVE barrier (knock-in from below)
     *  UpAndOut  — killed           when spot rises ABOVE barrier (knock-out from below)
     *  DownAndIn — activated only when spot falls BELOW barrier (knock-in from above)
     *  DownAndOut— killed           when spot falls BELOW barrier (knock-out from above)
     */
    enum class BarrierType
    {
        UpAndIn,
        UpAndOut,
        DownAndIn,
        DownAndOut
    };

    /**
     * Barrier option instrument.
     *
     * Underlying payoff is an European vanilla call or put.
     * Knock-in options pay the vanilla payoff only if the barrier was crossed.
     * Knock-out options pay the vanilla payoff only if the barrier was never crossed.
     * In both cases a rebate (default 0) is paid at expiry for the "dead" state.
     *
     * Barrier monitoring:
     *  - n_steps == 0  → daily monitoring (252 × T steps)
     *  - n_steps >  0  → explicitly specified number of monitoring steps
     *  - brownian_bridge == true → apply the Andersen–Brotherton-Ratcliffe
     *    Brownian-bridge correction for continuous-barrier approximation
     */
    struct BarrierOption final : Instrument
    {
        std::shared_ptr<const IPayoff> payoff;     ///< vanilla payoff (call/put, strike)
        std::shared_ptr<const IExercise> exercise; ///< European exercise date (maturity)
        BarrierType barrier_type;
        Real barrier;      ///< barrier level H
        Real rebate = 0.0; ///< paid at expiry if in "dead" state
        Real notional = 1.0;
        int n_steps = 0;             ///< 0 → use daily (252×T)
        bool brownian_bridge = true; ///< BB correction for continuous monitoring

        BarrierOption(std::shared_ptr<const IPayoff> p,
                      std::shared_ptr<const IExercise> e,
                      BarrierType bt,
                      Real H,
                      Real reb = 0.0,
                      Real ntl = 1.0)
            : payoff(std::move(p)), exercise(std::move(e)),
              barrier_type(bt), barrier(H), rebate(reb), notional(ntl)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif // INSTRUMENT_EQUITY_BARRIER_HPP
