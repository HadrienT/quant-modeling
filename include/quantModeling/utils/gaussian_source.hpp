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

    static_assert(GaussianSource<BoxMullerSource>);
    static_assert(GaussianSource<InverseNormalSource>);

} // namespace quantModeling

#endif // UTILS_GAUSSIAN_SOURCE_HPP
