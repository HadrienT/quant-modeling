#ifndef CORE_TYPES_HPP
#define CORE_TYPES_HPP
#include <stdexcept>

namespace quantModeling
{

    typedef double Time;
    typedef double DiscountFactor;
    typedef double Spread;
    typedef double Volatility;
    typedef double Probability;

    // ---------- Core types ----------
    using Real = double;
    using Time = double;

    // ---------- Error types ----------
    struct PricingError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct UnsupportedInstrument : PricingError
    {
        using PricingError::PricingError;
    };

    struct InvalidInput : PricingError
    {
        using PricingError::PricingError;
    };

}

#endif