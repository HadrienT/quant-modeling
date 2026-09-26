#ifndef UTILS_PHILOX_HPP
#define UTILS_PHILOX_HPP

#include <cstdint>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/utils/inverse_normal.hpp"

/**
 * @file philox.hpp
 * @brief Philox4x32-10 counter-based generator, host and device
 *        (blueprint/wp/19-gpu.md §2.3, ADR-G3).
 *
 * Salmon, Moraes, Dror & Shaw, "Parallel Random Numbers: As Easy as 1, 2, 3",
 * SC'11. A counter-based generator has no state: the output is a bijection of
 * (key, counter), so draw j of path p is a pure function of (seed, p, j).
 * That is what lets 10^5 GPU threads draw without coordination, makes the
 * result independent of how paths are split between threads, blocks or
 * devices, and lets an adjoint pass regenerate draws instead of storing them.
 *
 * Written here rather than taken from cuRAND or Random123 so that the CPU and
 * the GPU run the same integer arithmetic and therefore produce the same bits;
 * Random123's known-answer vectors are the oracle (tests/testPhilox.cpp).
 *
 * Layout used by the Monte-Carlo code:
 *   key     = (seed low 32 bits, seed high 32 bits)
 *   counter = (block of draws, 0, path low 32 bits, path high 32 bits)
 * One Philox call yields four 32-bit words = two 52-bit uniforms, so draws
 * 2b and 2b+1 of a path come from counter block b.
 */

namespace quantModeling
{

    struct Philox4x32
    {
        uint32_t v[4];
    };

    namespace philox_detail
    {
        constexpr uint32_t kM0 = 0xD2511F53u;
        constexpr uint32_t kM1 = 0xCD9E8D57u;
        constexpr uint32_t kW0 = 0x9E3779B9u; // golden ratio
        constexpr uint32_t kW1 = 0xBB67AE85u; // sqrt(3) - 1

        QM_HOST_DEVICE inline void mulhilo(uint32_t a, uint32_t b, uint32_t &hi, uint32_t &lo)
        {
            const uint64_t p = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
            hi = static_cast<uint32_t>(p >> 32);
            lo = static_cast<uint32_t>(p);
        }

        QM_HOST_DEVICE inline Philox4x32 round(const Philox4x32 &c, uint32_t k0, uint32_t k1)
        {
            uint32_t hi0, lo0, hi1, lo1;
            mulhilo(kM0, c.v[0], hi0, lo0);
            mulhilo(kM1, c.v[2], hi1, lo1);
            return Philox4x32{{hi1 ^ c.v[1] ^ k0, lo1, hi0 ^ c.v[3] ^ k1, lo0}};
        }
    } // namespace philox_detail

    /// Philox4x32 with 10 rounds (the Random123 default and the variant its
    /// known-answer tests cover).
    QM_HOST_DEVICE inline Philox4x32 philox4x32_10(Philox4x32 ctr, uint32_t k0, uint32_t k1)
    {
        using namespace philox_detail;
        for (int r = 0; r < 10; ++r)
        {
            if (r > 0)
            {
                k0 += kW0;
                k1 += kW1;
            }
            ctr = round(ctr, k0, k1);
        }
        return ctr;
    }

    /// Uniform in the open interval (0, 1) from two 32-bit words: 52 random
    /// bits m, mapped to (m + 1/2) / 2^52. Not 53 bits: (2^53 - 1) + 1/2 is
    /// not a double and rounds to 2^53, i.e. u = 1 and Φ⁻¹(u) = +inf, whereas
    /// every m + 1/2 below 2^52 is exact. The extreme draws are ±8.2σ.
    QM_HOST_DEVICE inline double philox_to_uniform(uint32_t a, uint32_t b)
    {
        constexpr double inv52 = 1.0 / 4503599627370496.0; // 2^-52
        const uint64_t m = (static_cast<uint64_t>(a >> 6) << 26) | static_cast<uint64_t>(b >> 6);
        return (static_cast<double>(m) + 0.5) * inv52;
    }

    /// The two uniforms of counter block `block` of path `path`.
    QM_HOST_DEVICE inline void philox_uniform_pair(uint64_t seed, uint64_t path, uint32_t block,
                                                   double &u0, double &u1)
    {
        const Philox4x32 ctr{{block, 0u, static_cast<uint32_t>(path), static_cast<uint32_t>(path >> 32)}};
        const Philox4x32 out = philox4x32_10(ctr, static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32));
        u0 = philox_to_uniform(out.v[0], out.v[1]);
        u1 = philox_to_uniform(out.v[2], out.v[3]);
    }

    /// Draw `j` of path `path`, as a pure function of (seed, path, j).
    QM_HOST_DEVICE inline double philox_uniform(uint64_t seed, uint64_t path, uint32_t j)
    {
        double u0, u1;
        philox_uniform_pair(seed, path, j >> 1, u0, u1);
        return (j & 1u) ? u1 : u0;
    }

    /**
     * @brief GaussianSource over one path's Philox draws (inverse CDF, never
     *        Box-Muller: Glasserman ch. 5.2, and one uniform per normal keeps
     *        draw j of the path at a fixed address).
     *
     * next() walks draws 0, 1, 2, ... of the current path, computing one
     * Philox block per two draws. set_path() jumps to another path in O(1):
     * there is no state to skip through.
     */
    struct PhiloxGaussianSource
    {
        uint64_t seed = 0;
        uint64_t path = 0;
        uint32_t j = 0;
        double spare = 0.0;

        QM_HOST_DEVICE PhiloxGaussianSource(uint64_t s, uint64_t p = 0)
            : seed(s), path(p) {}

        QM_HOST_DEVICE void set_path(uint64_t p)
        {
            path = p;
            j = 0;
        }

        QM_HOST_DEVICE double next()
        {
            double u;
            if (j & 1u)
            {
                u = spare;
            }
            else
            {
                philox_uniform_pair(seed, path, j >> 1, u, spare);
            }
            ++j;
            return inverse_normal_cdf(u);
        }
    };

} // namespace quantModeling

#endif // UTILS_PHILOX_HPP
