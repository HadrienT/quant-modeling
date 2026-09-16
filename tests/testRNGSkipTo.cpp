#include <gtest/gtest.h>

#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/rng_interface.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <cstdint>
#include <vector>

namespace quantModeling
{

    // ── Pcg32::skip -- the raw LCG jump-ahead ───────────────────────────────

    TEST(Pcg32Skip, MatchesDrawingSequentially)
    {
        for (uint64_t n : {0u, 1u, 2u, 7u, 63u, 64u, 65u, 1000u})
        {
            Pcg32 sequential(42, 7);
            for (uint64_t i = 0; i < n; ++i)
                sequential();

            Pcg32 skipped(42, 7);
            skipped.skip(n);

            EXPECT_EQ(sequential.state, skipped.state) << "n=" << n;
            // and the next draw from each matches too
            EXPECT_EQ(sequential(), skipped()) << "n=" << n;
        }
    }

    TEST(Pcg32Skip, LargeJumpMatchesTheSameLargeJumpDoneInTwoHalves)
    {
        // Exercises genuine binary exponentiation, not just small n: no test
        // here actually draws 2^40 values to check this the slow way, but
        // splitting one large jump into two and comparing against the
        // combined jump pins down that the O(log n) doubling composes
        // correctly regardless of how n is split.
        constexpr uint64_t total = (1ull << 40) + 12345;
        Pcg32 a(1, 3);
        a.skip(total);

        Pcg32 b(1, 3);
        b.skip(total / 2);
        b.skip(total - total / 2);

        EXPECT_EQ(a.state, b.state);
    }

    // ── SobolSequence::skip_to -- the Gray-code direct jump ────────────────

    TEST(SobolSkipTo, MatchesGeneratingSequentially)
    {
        for (uint32_t n : {0u, 1u, 2u, 3u, 8u, 63u, 64u, 65u, 500u})
        {
            constexpr int dim = 5;
            SobolSequence sequential(dim, /*scramble_seed=*/9);
            std::vector<double> discard(dim), expected(dim), actual(dim);
            for (uint32_t i = 0; i < n; ++i)
                sequential.next_uniform(discard);
            sequential.next_uniform(expected); // point n

            SobolSequence skipped(dim, /*scramble_seed=*/9);
            skipped.skip_to(n);
            skipped.next_uniform(actual); // should also be point n

            for (int d = 0; d < dim; ++d)
                EXPECT_DOUBLE_EQ(expected[d], actual[d]) << "n=" << n << " d=" << d;
        }
    }

    TEST(SobolSkipTo, ContinuesCorrectlyAfterASkip)
    {
        // skip_to() must leave the sequence in a state where *subsequent*
        // calls also stay correct, not just the very next one.
        constexpr int dim = 3;
        SobolSequence sequential(dim, 5);
        std::vector<double> discard(dim);
        for (int i = 0; i < 10; ++i)
            sequential.next_uniform(discard);
        std::vector<double> expected_a(dim), expected_b(dim);
        sequential.next_uniform(expected_a); // point 10
        sequential.next_uniform(expected_b); // point 11

        SobolSequence skipped(dim, 5);
        skipped.skip_to(10);
        std::vector<double> actual_a(dim), actual_b(dim);
        skipped.next_uniform(actual_a);
        skipped.next_uniform(actual_b);

        for (int d = 0; d < dim; ++d)
        {
            EXPECT_DOUBLE_EQ(expected_a[d], actual_a[d]) << "d=" << d;
            EXPECT_DOUBLE_EQ(expected_b[d], actual_b[d]) << "d=" << d;
        }
    }

    // ── RNG interface: Pcg32RNG and SobolRNG ────────────────────────────────

    TEST(Pcg32RNG, SkipToPathMatchesSequentialGeneration)
    {
        constexpr std::size_t dim = 4;
        Pcg32RNG sequential(11, 2);
        sequential.init(dim);
        std::vector<double> discard(dim), expected(dim), actual(dim);
        for (std::size_t p = 0; p < 5; ++p)
            sequential.next_g(discard);
        sequential.next_g(expected); // path 5

        Pcg32RNG skipped(11, 2);
        skipped.init(dim);
        skipped.skip_to(5);
        skipped.next_g(actual);

        EXPECT_EQ(expected, actual);
    }

    TEST(SobolRNG, SkipToPathMatchesSequentialGeneration)
    {
        constexpr std::size_t dim = 4;
        SobolRNG sequential(3);
        sequential.init(dim);
        std::vector<double> discard(dim), expected(dim), actual(dim);
        for (std::size_t p = 0; p < 5; ++p)
            sequential.next_g(discard);
        sequential.next_g(expected); // path 5

        SobolRNG skipped(3);
        skipped.init(dim);
        skipped.skip_to(5);
        skipped.next_g(actual);

        EXPECT_EQ(expected, actual);
    }

    TEST(Pcg32RNG, CloneIsIndependentOfTheOriginal)
    {
        Pcg32RNG rng(1, 1);
        rng.init(3);
        auto clone = rng.clone();

        std::vector<double> a(3), b(3);
        rng.next_u(a);
        clone->next_u(b);
        // Same seed/stream/state at clone time -> same first draw ...
        EXPECT_EQ(a, b);

        // ... but advancing one must not advance the other.
        std::vector<double> a2(3), b2(3);
        rng.next_u(a2);
        clone->next_u(b2);
        EXPECT_EQ(a2, b2); // still in lockstep: independent, identically-seeded copies
    }

    TEST(SobolRNG, CloneIsIndependentOfTheOriginal)
    {
        SobolRNG rng(4);
        rng.init(3);
        auto clone = rng.clone();

        std::vector<double> a(3), b(3);
        rng.next_u(a);
        clone->next_u(b);
        EXPECT_EQ(a, b);

        std::vector<double> a2(3), b2(3);
        rng.next_u(a2);
        clone->next_u(b2);
        EXPECT_EQ(a2, b2);
    }

} // namespace quantModeling
