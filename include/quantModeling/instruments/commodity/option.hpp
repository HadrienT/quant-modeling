#ifndef INSTRUMENT_COMMODITY_OPTION_HPP
#define INSTRUMENT_COMMODITY_OPTION_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief European commodity option (Black '76 on forward).
     *
     * Priced via Black's formula:
     *   c = df × [ F × N(d1) − K × N(d2) ]
     *   p = df × [ K × N(−d2) − F × N(−d1) ]
     * where F = S0 × exp((r + u − y) × T) is the forward price.
     */
    struct CommodityOption final : Instrument
    {
        Real strike;   ///< option strike K
        Time maturity; ///< expiry T
        bool is_call;  ///< true = call
        Real notional = 1.0;

        CommodityOption(Real K, Time T, bool call, Real N = 1.0)
            : strike(K), maturity(T), is_call(call), notional(N) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
