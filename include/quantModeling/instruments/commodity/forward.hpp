#ifndef INSTRUMENT_COMMODITY_FORWARD_HPP
#define INSTRUMENT_COMMODITY_FORWARD_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    /**
     * @brief Commodity forward.
     *
     * Cost-of-carry model:  F(0,T) = S0 × exp((r + u − y) × T)
     *   r = risk-free rate
     *   u = storage cost rate (annualised)
     *   y = convenience yield (annualised)
     *
     * PV = notional × (F(0,T) − K) × exp(−r × T)
     */
    struct CommodityForward final : Instrument
    {
        Real strike;   ///< delivery price K
        Time maturity; ///< delivery date
        Real notional = 1.0;

        CommodityForward(Real K, Time T, Real N = 1.0)
            : strike(K), maturity(T), notional(N) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
