#ifndef INSTRUMENT_EQUITY_FUTURE_HPP
#define INSTRUMENT_EQUITY_FUTURE_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    struct EquityFuture final : Instrument
    {
        Real strike;
        Time maturity;
        Real notional = 1.0;

        EquityFuture(Real strike_, Time maturity_, Real notional_ = 1.0)
            : strike(strike_), maturity(maturity_), notional(notional_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
