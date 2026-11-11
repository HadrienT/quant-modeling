#ifndef UTILS_INVERSE_NORMAL_HPP
#define UTILS_INVERSE_NORMAL_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/utils/fused.hpp"

namespace quantModeling
{

    /**
     * @brief Inverse of the standard normal CDF, Φ⁻¹(p), by Wichura's
     *        algorithm AS 241, PPND16 (Applied Statistics 37(3), 1988,
     *        477-484): rational functions of degree 7 in three regions, a log
     *        and a sqrt in the tails only, one pass.
     *
     * Why this instead of Box-Muller:
     *  - required for quasi-Monte-Carlo (Sobol): the monotone map u → z
     *    preserves the low-discrepancy structure, Box-Muller does not;
     *  - stateless and branch-light: one sample per uniform, no spare
     *    caching → trivially vectorizable and CUDA-friendly;
     *  - same API on CPU and GPU (QM_HOST_DEVICE).
     *
     * Why AS 241 (WP 26 §5, P1i, adopted 2026-11-11). Against 45 289 values
     * at 45+ digits (scripts/gen_inverse_normal_reference.py, a sample in
     * tests/data) it stays within 6 ulp from 1e-300 to 1 - 2^-53, CPU and
     * GPU. It replaced Acklam's approximation plus a Halley step, which was
     * accurate to only ~1e-9 in its upper tail (the correction
     * 0.5 erfc(-x/√2) - p cancels for p near 1) and paid an erfc and an exp
     * on every draw: on one V100 (ncu) the compiled script kernels went
     * 9.76 -> 6.23 ms (up-and-out BS, 2.26 M paths) and 29.95 -> 21.64 ms
     * (Asian local vol, 0.95 M); the interpreter 26.87 -> 14.60 and
     * 59.91 -> 41.05. Coefficients from the paper, as GRASS GIS's as241.c
     * and R's qnorm.c reproduce them; the paper's hash sums of their
     * mantissas check the transcription.
     *
     * On the GPU: called, not inlined, and its polynomials fused
     * (utils/fused.hpp, WP 26 P1h): the script kernels are bound by the
     * instruction cache.
     *
     * @param p probability in (0, 1); 0 and 1 give -inf and +inf, NaN gives
     *          NaN.
     */
    QM_HOST_DEVICE QM_DEVICE_NOINLINE inline double inverse_normal_cdf(double p)
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
