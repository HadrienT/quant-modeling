#ifndef UTILS_ACCUMULATORS_HPP
#define UTILS_ACCUMULATORS_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

namespace quantModeling
{

    /**
     * @brief Welford online mean/variance accumulator.
     *
     * Numerically stable single-pass estimator; O(1) memory regardless of
     * the number of samples. Centralizes the pattern previously duplicated
     * in every MC engine.
     */
    struct WelfordAccumulator
    {
        Real mean = 0.0;
        Real m2 = 0.0;
        long long n = 0;

        QM_HOST_DEVICE void add(Real x)
        {
            ++n;
            const Real delta = x - mean;
            mean += delta / static_cast<Real>(n);
            const Real delta2 = x - mean;
            m2 += delta * delta2;
        }

        /// Unbiased sample variance (0 if fewer than 2 samples).
        QM_HOST_DEVICE Real variance() const
        {
            return (n > 1) ? m2 / static_cast<Real>(n - 1) : Real(0);
        }

        /// Standard error of the mean (0 if fewer than 2 samples).
        QM_HOST_DEVICE Real std_error() const
        {
            return (n > 1) ? std::sqrt(variance() / static_cast<Real>(n)) : Real(0);
        }

        /// Merge another accumulator (Chan's parallel formula) — used to
        /// combine per-thread/per-GPU-block partial results.
        QM_HOST_DEVICE void merge(const WelfordAccumulator &other)
        {
            if (other.n == 0)
                return;
            if (n == 0)
            {
                *this = other;
                return;
            }
            const Real total = static_cast<Real>(n + other.n);
            const Real delta = other.mean - mean;
            mean += delta * static_cast<Real>(other.n) / total;
            m2 += other.m2 +
                  delta * delta * static_cast<Real>(n) * static_cast<Real>(other.n) / total;
            n += other.n;
        }
    };

} // namespace quantModeling

#endif // UTILS_ACCUMULATORS_HPP
