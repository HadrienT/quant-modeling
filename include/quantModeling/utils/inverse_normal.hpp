#ifndef UTILS_INVERSE_NORMAL_HPP
#define UTILS_INVERSE_NORMAL_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/utils/fused.hpp"

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
    // On the GPU: called, not inlined, and its multiply-adds fused
    // (blueprint/wp/26-performance.md §5, P1h). The script kernels are bound
    // by the instruction cache, and Φ⁻¹ with its erfc and exp was inlined at
    // every draw. Measured on one V100 (ncu, 2026-11-11), kernel time:
    // compiled scripts 12.09 -> 10.42 ms (up-and-out BS, 2.26 M paths) and
    // 34.12 -> 29.96 ms (Asian local vol, 0.95 M); the interpreter 29.29 ->
    // 27.00 and 61.30 -> 60.21; duals and adjoint within noise. Not inlining
    // alone gives most of it (11.00, 31.51); the FMA adds 5 % to the compiled
    // scripts and costs 5 % to the interpreter. CPU bits unchanged.
    QM_HOST_DEVICE QM_DEVICE_NOINLINE inline double inverse_normal_cdf(double p)
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
            x = horner(q, c1, c2, c3, c4, c5, c6) / horner(q, d1, d2, d3, d4, 1.0);
        }
        else if (p <= p_high)
        {
            // Central region
            const double q = p - 0.5;
            const double r = q * q;
            x = horner(r, a1, a2, a3, a4, a5, a6) * q / horner(r, b1, b2, b3, b4, b5, 1.0);
        }
        else
        {
            // Upper tail
            const double q = std::sqrt(-2.0 * std::log(1.0 - p));
            x = -horner(q, c1, c2, c3, c4, c5, c6) / horner(q, d1, d2, d3, d4, 1.0);
        }

        // One Halley refinement step: brings the result to ~full double precision.
        constexpr double sqrt_2pi = 2.506628274631000502415765284811;
        constexpr double inv_sqrt2 = 0.70710678118654752440084436210485;
        // The polynomials and the multiply-adds below fuse on the device
        // (utils/fused.hpp, WP 26 P1h); on the host they are the expressions
        // they replace.
        const double e = mul_add(0.5, std::erfc(-x * inv_sqrt2), -p);
        const double u = e * sqrt_2pi * std::exp(0.5 * x * x);
        x = x - u / mul_add(0.5 * x, u, 1.0);

        return x;
    }

    /**
     * @brief Φ⁻¹(p) by Wichura's algorithm AS 241, PPND16 (Applied
     *        Statistics 37(3), 1988, 477-484): "accurate to about 1 part in
     *        10^16", in one pass -- rational functions of degree 7 in three
     *        regions, a log and a sqrt in the tails only, no erfc and no
     *        refinement.
     *
     * Candidate replacement for inverse_normal_cdf (WP 26 §5, P1i), under
     * evaluation: not used by any engine. Coefficients from the paper, as
     * GRASS GIS's as241.c and R's qnorm.c reproduce them; the hash sums of
     * their mantissas are checked by the accuracy measurement.
     */
    QM_HOST_DEVICE QM_DEVICE_NOINLINE inline double inverse_normal_cdf_as241(double p)
    {
        const double q = p - 0.5;
        if (std::fabs(q) <= 0.425)
        {
            const double r = 0.180625 - q * q;
            return q *
                   horner(r, 2.5090809287301226727e+3, 3.3430575583588128105e+4, 6.7265770927008700853e+4,
                          4.5921953931549871457e+4, 1.3731693765509461125e+4, 1.9715909503065514427e+3,
                          1.3314166789178437745e+2, 3.3871328727963666080e0) /
                   horner(r, 5.2264952788528545610e+3, 2.8729085735721942674e+4, 3.9307895800092710610e+4,
                          2.1213794301586595867e+4, 5.3941960214247511077e+3, 6.8718700749205790830e+2,
                          4.2313330701600911252e+1, 1.0);
        }
        double r = q < 0.0 ? p : 1.0 - p;
        if (r <= 0.0)
            return q < 0.0 ? -INFINITY : INFINITY;
        r = std::sqrt(-std::log(r));
        double x;
        if (r <= 5.0)
        {
            r -= 1.6;
            x = horner(r, 7.74545014278341407640e-4, 2.27238449892691845833e-2, 2.41780725177450611770e-1,
                       1.27045825245236838258e0, 3.64784832476320460504e0, 5.76949722146069140550e0,
                       4.63033784615654529590e0, 1.42343711074968357734e0) /
                horner(r, 1.05075007164441684324e-9, 5.47593808499534494600e-4, 1.51986665636164571966e-2,
                       1.48103976427480074590e-1, 6.89767334985100004550e-1, 1.67638483018380384940e0,
                       2.05319162663775882187e0, 1.0);
        }
        else
        {
            r -= 5.0;
            x = horner(r, 2.01033439929228813265e-7, 2.71155556874348757815e-5, 1.24266094738807843860e-3,
                       2.65321895265761230930e-2, 2.96560571828504891230e-1, 1.78482653991729133580e0,
                       5.46378491116411436990e0, 6.65790464350110377720e0) /
                horner(r, 2.04426310338993978564e-15, 1.42151175831644588870e-7, 1.84631831751005468180e-5,
                       7.86869131145613259100e-4, 1.48753612908506148525e-2, 1.36929880922735805310e-1,
                       5.99832206555887937690e-1, 1.0);
        }
        return q < 0.0 ? -x : x;
    }

} // namespace quantModeling

#endif // UTILS_INVERSE_NORMAL_HPP
