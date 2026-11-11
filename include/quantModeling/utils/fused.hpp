#ifndef QM_UTILS_FUSED_HPP
#define QM_UTILS_FUSED_HPP

#include "quantModeling/core/platform.hpp"

/**
 * @file fused.hpp
 * @brief Multiply-adds fused where the source says so
 *        (blueprint/wp/26-performance.md §5, lot P1h).
 *
 * The GPU script kernels are built without FMA contraction (--fmad=false):
 * with it, whether `a * b + c` fuses depends on how the optimiser sees the
 * code, and a compiled script could not give the interpreter's bits (P1b).
 * Contraction was worth 5-7 % of a path. These helpers take that back at
 * the places we choose: on the device, `mul_add` is one FMA (one rounding);
 * the interpreter and the compiled scripts include the same code, so they
 * fuse at the same places and keep the same bits.
 *
 * On the host, `mul_add(a, b, c)` is `a * b + c`, the expression it
 * replaces: CPU results do not move. (The wheel is built without
 * -march=native, where std::fma would be emulated in software.)
 *
 * Used where it was measured to pay: Φ⁻¹ (utils/inverse_normal.hpp). Fusing
 * the model steps' multiply-adds as well gained under 1 % on the compiled
 * scripts and cost 3 % on the interpreter (2026-11-11): not kept.
 */

namespace quantModeling
{

    /// a * b + c: one FMA on the device, the plain expression on the host.
    QM_HOST_DEVICE inline double mul_add(double a, double b, double c)
    {
#ifdef __CUDA_ARCH__
        return fma(a, b, c);
#else
        return a * b + c;
#endif
    }

    /// The polynomial c0 x^n + c1 x^(n-1) + ... + cn by Horner's rule,
    /// ((c0 x + c1) x + c2) x + ..., one mul_add per coefficient after the
    /// first.
    template <class... C>
    QM_HOST_DEVICE inline double horner(double x, double c0, C... cs)
    {
        double acc = c0;
        ((acc = mul_add(acc, x, static_cast<double>(cs))), ...);
        return acc;
    }

} // namespace quantModeling

#endif // QM_UTILS_FUSED_HPP
