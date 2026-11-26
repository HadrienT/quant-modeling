#ifndef MARKET_ESSVI_DETAIL_HPP
#define MARKET_ESSVI_DETAIL_HPP

// What the eSSVI calibration (essvi.cpp) and its superbucket
// (essvi_superbucket.cpp) share, and nothing else includes.

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi_calibration.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling::essvi_detail
{
    /// The strict butterfly bound psi (1 + |rho|) < 4, met with a margin.
    inline constexpr Real kStrict = 1.0 - 1e-12;

    /// The quotes the at-the-money estimate reads: the seven (or all)
    /// nearest k = 0, in that order.
    std::vector<std::size_t> atm_quotes(const std::vector<SVISliceQuote> &quotes);

    /// The quotes' at-the-money total variance: a weighted quadratic fit of
    /// w = iv^2 T on k through atm_quotes, its value at 0 -- robust to one
    /// noisy quote. `std_error`: the estimate's noise, the residuals'
    /// weighted spread over sqrt(count), floored at 1e-3 theta + 1e-8.
    Real atm_total_variance(const std::vector<SVISliceQuote> &quotes, Real ttm, Real *std_error = nullptr);
} // namespace quantModeling::essvi_detail

#endif
