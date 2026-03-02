#ifndef INSTRUMENT_RATES_CAPLET_HPP
#define INSTRUMENT_RATES_CAPLET_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief Standalone interest-rate caplet or floorlet.
     *
     * A caplet pays at T_end:
     *   N × δ × max(L(T_start, T_end) − K, 0)
     *
     * where L is the simply-compounded forward rate over [T_start, T_end],
     *       δ = T_end − T_start is the accrual period,
     *       K is the strike rate.
     *
     * Under a short-rate model, this decomposes into a put on a ZCB:
     *   Caplet = (1 + Kδ) × Put(K_p, T_start, T_end)
     *   K_p = 1 / (1 + Kδ)
     *
     * A floorlet is the corresponding call on the ZCB.
     */
    struct Caplet final : Instrument
    {
        Time start;         ///< T_start — fixing date
        Time end;           ///< T_end   — payment date
        Real strike;        ///< cap/floor strike rate K
        bool is_cap = true; ///< true = caplet, false = floorlet
        Real notional = 1.0;

        Caplet(Time t_start, Time t_end, Real K,
               bool cap = true, Real ntl = 1.0)
            : start(t_start), end(t_end), strike(K),
              is_cap(cap), notional(ntl)
        {
        }

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
