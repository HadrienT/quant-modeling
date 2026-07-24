#ifndef UTILS_GAUSSIAN_SOURCE_HPP
#define UTILS_GAUSSIAN_SOURCE_HPP

#include <concepts>

#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"

namespace quantModeling
{

    /**
     * @brief Compile-time interface for N(0,1) sample sources.
     *
     * Monte-Carlo kernels are templated on this concept instead of calling a
     * virtual generator: the compiler inlines the whole draw into the path
     * loop (zero virtual dispatch in the hot path), and new sources
     * (Sobol + inverse normal, Philox, ...) plug in without touching engines.
     */
    template <class G>
    concept GaussianSource = requires(G g) {
        { g.next() } -> std::convertible_to<double>;
    };

    /// PCG32 + Box-Muller. Reproduces the historical draw sequence exactly.
    struct BoxMullerSource
    {
        Pcg32 rng;
        NormalBoxMuller bm{};

        explicit BoxMullerSource(Pcg32 r) : rng(r) {}

        double next() { return bm(rng); }
    };

    /// PCG32 + inverse normal CDF. Stateless transform (one uniform per
    /// sample), branch-light — the pattern QMC and CUDA backends will use.
    struct InverseNormalSource
    {
        Pcg32 rng;

        explicit InverseNormalSource(Pcg32 r) : rng(r) {}

        double next() { return inverse_normal_cdf(uniform01(rng)); }
    };

    /**
     * @brief Stratified N(0,1) source: the i-th of n draws is
     *        Φ⁻¹((i + η_i)/n) with fresh jitter η_i ~ U[0,1).
     *
     * Every stratum of the unit interval contributes exactly one sample, so
     * the sampling variance of the *mean* drops far below 1/n for smooth
     * integrands (proportional stratification). Draws are NOT i.i.d.: use
     * independent batches (one jitter seed per batch) for error bars, like
     * RQMC. Total draws must not exceed n_strata.
     */
    struct StratifiedGaussianSource
    {
        Pcg32 rng;
        int n_strata;
        int i = 0;

        StratifiedGaussianSource(Pcg32 r, int n) : rng(r), n_strata(n) {}

        double next()
        {
            const double u = (static_cast<double>(i++) + uniform01(rng)) /
                             static_cast<double>(n_strata);
            return inverse_normal_cdf(u);
        }
    };

    static_assert(GaussianSource<BoxMullerSource>);
    static_assert(GaussianSource<InverseNormalSource>);
    static_assert(GaussianSource<StratifiedGaussianSource>);

} // namespace quantModeling

#endif // UTILS_GAUSSIAN_SOURCE_HPP
