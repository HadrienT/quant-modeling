#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "quantModeling/utils/philox.hpp"

namespace quantModeling
{

    // ---------------------------------------------------------------------
    // Philox4x32-10 (blueprint/wp/19-gpu.md §2.3, §10)
    // ---------------------------------------------------------------------

    // Known-answer vectors from Random123 (kat_vectors, philox4x32 10 rounds):
    // counter words, key words -> output words. Matching all three pins the
    // multipliers, the Weyl key schedule and the word permutation.
    TEST(Philox, Random123KnownAnswers)
    {
        struct Kat
        {
            Philox4x32 ctr;
            uint32_t k0, k1;
            Philox4x32 expected;
        };
        const Kat kats[] = {
            {{{0u, 0u, 0u, 0u}}, 0u, 0u, {{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u}}},
            {{{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}},
             0xffffffffu,
             0xffffffffu,
             {{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu}}},
            {{{0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u}},
             0xa4093822u,
             0x299f31d0u,
             {{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u}}},
        };
        for (const Kat &kat : kats)
        {
            const Philox4x32 out = philox4x32_10(kat.ctr, kat.k0, kat.k1);
            for (int i = 0; i < 4; ++i)
                EXPECT_EQ(out.v[i], kat.expected.v[i]) << "word " << i;
        }
    }

    TEST(Philox, UniformStaysInsideOpenInterval)
    {
        EXPECT_GT(philox_to_uniform(0u, 0u), 0.0);
        EXPECT_LT(philox_to_uniform(0xffffffffu, 0xffffffffu), 1.0);
        EXPECT_TRUE(std::isfinite(inverse_normal_cdf(philox_to_uniform(0u, 0u))));
        EXPECT_TRUE(std::isfinite(inverse_normal_cdf(philox_to_uniform(0xffffffffu, 0xffffffffu))));
    }

    // Draw j of path p is a pure function of (seed, p, j): the source walking
    // a path gives exactly the addressed draws, in any visiting order.
    TEST(Philox, SourceMatchesAddressedDraws)
    {
        constexpr uint64_t seed = 0x1234'5678'9abcull;
        PhiloxGaussianSource src(seed);
        for (uint64_t p : {uint64_t{7}, uint64_t{0}, uint64_t{1} << 40, uint64_t{7}})
        {
            src.set_path(p);
            for (uint32_t j = 0; j < 9; ++j)
                EXPECT_EQ(src.next(), inverse_normal_cdf(philox_uniform(seed, p, j)));
        }
    }

    TEST(Philox, SeedsAndPathsGiveDistinctStreams)
    {
        EXPECT_NE(philox_uniform(1, 0, 0), philox_uniform(2, 0, 0));
        EXPECT_NE(philox_uniform(1, 0, 0), philox_uniform(1, 1, 0));
        // The high word of the path index is part of the counter.
        EXPECT_NE(philox_uniform(1, 5, 0), philox_uniform(1, 5 + (uint64_t{1} << 32), 0));
        EXPECT_NE(philox_uniform(1, 0, 0), philox_uniform(1, 0, 1));
    }

    // Moments of U(0,1) and N(0,1), and no lag-1 correlation between
    // consecutive paths' first draws, at 4 standard errors.
    TEST(Philox, MomentsAndCrossPathIndependence)
    {
        constexpr int n = 1 << 20;
        constexpr uint64_t seed = 42;
        double su = 0, suu = 0, sz = 0, szz = 0, szz_lag = 0;
        double prev = inverse_normal_cdf(philox_uniform(seed, 0, 0));
        for (int i = 0; i < n; ++i)
        {
            const double u = philox_uniform(seed, static_cast<uint64_t>(i), 1);
            su += u;
            suu += u * u;
            const double z = inverse_normal_cdf(philox_uniform(seed, static_cast<uint64_t>(i) + 1, 0));
            sz += z;
            szz += z * z;
            szz_lag += z * prev;
            prev = z;
        }
        const double N = n;
        EXPECT_NEAR(su / N, 0.5, 4.0 * std::sqrt(1.0 / 12.0 / N));
        EXPECT_NEAR(suu / N - (su / N) * (su / N), 1.0 / 12.0, 4.0 * std::sqrt(1.0 / 180.0 / N));
        EXPECT_NEAR(sz / N, 0.0, 4.0 / std::sqrt(N));
        EXPECT_NEAR(szz / N, 1.0, 4.0 * std::sqrt(2.0 / N));
        EXPECT_NEAR(szz_lag / N, 0.0, 4.0 / std::sqrt(N));
    }

} // namespace quantModeling
