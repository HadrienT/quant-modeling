#ifndef QM_TESTS_SUPPORT_CPU_GPU_TOLERANCE_HPP
#define QM_TESTS_SUPPORT_CPU_GPU_TOLERANCE_HPP

#include <algorithm>
#include <cmath>

/**
 * @file cpu_gpu_tolerance.hpp
 * @brief How closely two implementations of the same estimator must agree on
 *        the same draws (the maintainer's decision of 2026-11-16, the
 *        hpc-finance skill's rule 1): CPU and GPU, the compiled and the
 *        interpreted script, a fused and an unfused build. Not the bits --
 *        FMA, the device libm and reassociated sums move the last ones -- but
 *        the rounding an industry pricer accepts: 1e-12 relative on a price,
 *        1e-10 on a risk (longer sums, adjoint accumulation). One
 *        configuration run twice still gives the same bits (determinism).
 */

namespace quantModeling::test_support
{
    inline constexpr double kCpuGpuPriceRel = 1e-12;
    inline constexpr double kCpuGpuRiskRel = 1e-10;

    /// |a - b| allowed for a price whose scale is `scale` (at least 1, so a
    /// price near 0 is compared absolutely).
    inline double price_tolerance(double scale)
    {
        return kCpuGpuPriceRel * std::max(1.0, std::abs(scale));
    }

    /// |a - b| allowed for a risk, relative to the largest risk of its vector.
    inline double risk_tolerance(double largest)
    {
        return kCpuGpuRiskRel * std::max(1e-12, std::abs(largest));
    }
} // namespace quantModeling::test_support

#endif // QM_TESTS_SUPPORT_CPU_GPU_TOLERANCE_HPP
