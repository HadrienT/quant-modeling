#ifndef INSTRUMENT_FX_FORWARD_HPP
#define INSTRUMENT_FX_FORWARD_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief FX forward: agree to exchange notional at rate K at time T.
     *
     * PV = notional × [ S0 × exp(−r_f × T) − K × exp(−r_d × T) ]
     * where S0 is the spot exchange rate (domestic per foreign).
     */
    struct FXForward final : Instrument
    {
        Real strike;   ///< delivery rate K
        Time maturity; ///< delivery date
        Real notional = 1.0;

        FXForward(Real K, Time T, Real N = 1.0)
            : strike(K), maturity(T), notional(N) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
