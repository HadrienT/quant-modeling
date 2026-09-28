#ifndef QM_ENGINES_MC_PATH_DRAWS_HPP
#define QM_ENGINES_MC_PATH_DRAWS_HPP

#include <cmath>
#include <cstdint>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"

/**
 * @file path_draws.hpp
 * @brief One path's gaussians, counter-based, plain or with a stratified
 *        terminal value -- host and device (blueprint/wp/19-gpu.md §2.5,
 *        lot G3).
 *
 * Plain: draw j of unit u is Phi^-1(Philox(seed, u, j)), the generator of the
 * GPU engines and of the CPU generic engine with mc_rng = Philox.
 *
 * Stratified (Glasserman 2004, §4.3.2, "stratifying the terminal value of a
 * Brownian path"): the first Brownian factor's terminal value W(T) is drawn
 * in stratum i of m equiprobable strata,
 *
 *     W(T) = sqrt(T) Phi^-1((i + U) / m),        U ~ U(0, 1),
 *
 * and the rest of that factor's path is filled in *conditionally* on it, one
 * step at a time, by the Brownian bridge from (t_prev, W(t_prev)) to (T, W(T)):
 *
 *     W(t) = W(t_prev) + (t - t_prev) / (T - t_prev) (W(T) - W(t_prev))
 *            + sqrt((t - t_prev)(T - t) / (T - t_prev)) xi,   xi ~ N(0, 1).
 *
 * The path has exactly the law of a Brownian path (the terminal value's law
 * is only re-sampled by strata), so the model is untouched: it still
 * receives its normalised increments (W(t) - W(t_prev)) / sqrt(t - t_prev) in
 * time order. Unlike a full bisection bridge, this needs O(1) memory per path
 * -- the device keeps no per-step array. Every other draw (the other
 * factors, jump draws) stays plain.
 *
 * The first factor's terminal value is what most payoffs depend on first
 * (a call, a digital, an autocall's final test); proportional allocation,
 * one path per stratum, never increases the variance (Glasserman eq. 4.46).
 * Stratified points are not independent, so the error is measured over
 * independent replicates, each of m strata.
 */

namespace quantModeling::mc
{

    struct PathDraws
    {
        uint64_t seed = 1;
        int stride = 1; ///< draws per drawing step; the first factor is slot 0

        // Stratification (strata == 0: plain draws).
        uint64_t strata = 0;
        const Time *times = nullptr; ///< end of each drawing step
        int n_steps = 0;             ///< drawing steps

        // Per path.
        uint64_t unit = 0;
        double W_T = 0.0, W_prev = 0.0, t_prev = 0.0;

        QM_HOST_DEVICE double plain(uint32_t j) const
        {
            return inverse_normal_cdf(philox_uniform(seed, unit, j));
        }

        /// Start unit `u` (its Philox counter); `stratum` < strata when stratified.
        QM_HOST_DEVICE void begin(uint64_t u, uint64_t stratum = 0)
        {
            unit = u;
            W_prev = 0.0;
            t_prev = 0.0;
            if (strata == 0 || n_steps == 0)
                return;
            const double U = philox_uniform(seed, u, static_cast<uint32_t>((n_steps - 1) * stride));
            double p = (static_cast<double>(stratum) + U) / static_cast<double>(strata);
            // (m - 1 + U) / m rounds to 1 when U is within an ulp of 1.
            const double top = 1.0 - 0x1p-53;
            if (p > top)
                p = top;
            W_T = std::sqrt(times[n_steps - 1]) * inverse_normal_cdf(p);
        }

        /// Draw f of drawing step d. Steps must come in increasing order
        /// (each model reads its draws in time order).
        QM_HOST_DEVICE double operator()(int d, int f)
        {
            const auto j = static_cast<uint32_t>(d * stride + f);
            if (strata == 0 || f != 0)
                return plain(j);
            const double t = times[d];
            const double T = times[n_steps - 1];
            double W;
            if (d == n_steps - 1)
                W = W_T;
            else
            {
                const double span = T - t_prev;
                W = W_prev + (t - t_prev) / span * (W_T - W_prev) + std::sqrt((t - t_prev) * (T - t) / span) * plain(j);
            }
            const double z = (W - W_prev) / std::sqrt(t - t_prev);
            W_prev = W;
            t_prev = t;
            return z;
        }
    };

    /**
     * @brief The stratum of unit u among m (a bijection of [0, m)).
     *
     * Inside every full logical block of 4 096 units, the thread that
     * reduces units t, t + 256, ... (engines/mc/logical_blocks.hpp) gets 16
     * *consecutive* strata, in order: its successive paths are neighbours,
     * which is what the within-stratum control slope needs
     * (StratifiedControlAccumulator). A partial last block keeps u.
     */
    QM_HOST_DEVICE inline uint64_t stratum_of(uint64_t u, uint64_t m)
    {
        constexpr uint64_t kBlock = 4096, kThreads = 256, kPerThread = 16;
        const uint64_t block = u / kBlock;
        if ((block + 1) * kBlock > m)
            return u;
        const uint64_t l = u - block * kBlock;
        return block * kBlock + (l % kThreads) * kPerThread + l / kThreads;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_PATH_DRAWS_HPP
