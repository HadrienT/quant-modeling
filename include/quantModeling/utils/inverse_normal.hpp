#ifndef UTILS_INVERSE_NORMAL_HPP
#define UTILS_INVERSE_NORMAL_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"

namespace quantModeling
{

    /**
     * @brief Inverse of the standard normal CDF, Φ⁻¹(p).
     *
     * Peter Acklam's rational approximation (max relative error ~1.15e-9)
     * followed by one Halley refinement step, giving accuracy close to
     * machine precision over the full open interval (0, 1).
     *
     * Why this instead of Box-Muller:
     *  - required for quasi-Monte-Carlo (Sobol): the monotone map u → z
     *    preserves the low-discrepancy structure, Box-Muller does not;
     *  - stateless and branch-light: one sample per uniform, no spare
     *    caching → trivially vectorizable and CUDA-friendly;
     *  - same API on CPU and GPU (QM_HOST_DEVICE).
     *
     * @param p probability in (0, 1). Values outside return ±infinity/NaN
     *          consistent with the mathematical limit.
     */
    QM_HOST_DEVICE inline double inverse_normal_cdf(double p)
    {
        // Coefficients for the central region rational approximation
        constexpr double a1 = -3.969683028665376e+01;
        constexpr double a2 = 2.209460984245205e+02;
        constexpr double a3 = -2.759285104469687e+02;
        constexpr double a4 = 1.383577518672690e+02;
        constexpr double a5 = -3.066479806614716e+01;
        constexpr double a6 = 2.506628277459239e+00;

        constexpr double b1 = -5.447609879822406e+01;
        constexpr double b2 = 1.615858368580409e+02;
        constexpr double b3 = -1.556989798598866e+02;
        constexpr double b4 = 6.680131188771972e+01;
        constexpr double b5 = -1.328068155288572e+01;

        // Coefficients for the tail regions
        constexpr double c1 = -7.784894002430293e-03;
        constexpr double c2 = -3.223964580411365e-01;
        constexpr double c3 = -2.400758277161838e+00;
        constexpr double c4 = -2.549732539343734e+00;
        constexpr double c5 = 4.374664141464968e+00;
        constexpr double c6 = 2.938163982698783e+00;

        constexpr double d1 = 7.784695709041462e-03;
        constexpr double d2 = 3.224671290700398e-01;
        constexpr double d3 = 2.445134137142996e+00;
        constexpr double d4 = 3.754408661907416e+00;

        constexpr double p_low = 0.02425;
        constexpr double p_high = 1.0 - p_low;

        double x;
        if (p < p_low)
        {
            // Lower tail
            const double q = std::sqrt(-2.0 * std::log(p));
            x = (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
                ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
        }
        else if (p <= p_high)
        {
            // Central region
            const double q = p - 0.5;
            const double r = q * q;
            x = (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
                (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
        }
        else
        {
            // Upper tail
            const double q = std::sqrt(-2.0 * std::log(1.0 - p));
            x = -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
                ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
        }

        // One Halley refinement step: brings the result to ~full double precision.
        constexpr double sqrt_2pi = 2.506628274631000502415765284811;
        constexpr double inv_sqrt2 = 0.70710678118654752440084436210485;
        const double e = 0.5 * std::erfc(-x * inv_sqrt2) - p;
        const double u = e * sqrt_2pi * std::exp(0.5 * x * x);
        x = x - u / (1.0 + 0.5 * x * u);

        return x;
    }

} // namespace quantModeling

#endif // UTILS_INVERSE_NORMAL_HPP
