#ifndef INSTRUMENT_RATES_ZERO_COUPON_BOND_HPP
#define INSTRUMENT_RATES_ZERO_COUPON_BOND_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    struct ZeroCouponBond final : Instrument
    {
        Time maturity;
        Real notional = 1.0;

        ZeroCouponBond(Time maturity_, Real notional_ = 1.0)
            : maturity(maturity_), notional(notional_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
