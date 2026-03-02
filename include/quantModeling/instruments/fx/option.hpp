#ifndef INSTRUMENT_FX_OPTION_HPP
#define INSTRUMENT_FX_OPTION_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief European FX option (Garman-Kohlhagen).
     *
     * Priced identically to BS with:
     *   S0 = spot FX rate (domestic per foreign)
     *   r  = domestic risk-free rate
     *   q  = foreign risk-free rate (acts as continuous dividend yield)
     *   σ  = FX volatility
     */
    struct FXOption final : Instrument
    {
        Real strike;   ///< option strike K
        Time maturity; ///< expiry T
        bool is_call;  ///< true = call on foreign ccy
        Real notional = 1.0;

        FXOption(Real K, Time T, bool call, Real N = 1.0)
            : strike(K), maturity(T), is_call(call), notional(N) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
