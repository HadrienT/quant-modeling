#ifndef INSTRUMENT_RATES_FIXED_RATE_BOND_HPP
#define INSTRUMENT_RATES_FIXED_RATE_BOND_HPP

#include "quantModeling/instruments/base.hpp"

namespace quantModeling
{

    struct FixedRateBond final : Instrument
    {
        Time maturity;
        Real coupon_rate;
        int coupon_frequency = 1;
        Real notional = 1.0;

        FixedRateBond(Real coupon_rate_, Time maturity_, int coupon_frequency_ = 1, Real notional_ = 1.0)
            : maturity(maturity_), coupon_rate(coupon_rate_), coupon_frequency(coupon_frequency_), notional(notional_) {}

        void accept(IInstrumentVisitor &v) const override { v.visit(*this); }
    };

} // namespace quantModeling

#endif
