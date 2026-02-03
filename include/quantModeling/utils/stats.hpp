#ifndef stats_hpp
#define stats_hpp
#include <cmath>
#include <quantModeling/core/types.hpp>
namespace quantModeling {
inline Real norm_pdf(Real x) {
  static constexpr Real inv_sqrt_2pi =
      0.39894228040143267793994605993438; // 1/sqrt(2π)
  return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

inline Real norm_cdf(Real x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
} // namespace quantModeling
#endif