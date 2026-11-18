#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/stats.hpp"

// Owen's scrambling of the Sobol points (utils/sobol.hpp, owen_scramble): it
// is a nested random permutation, it keeps the net property that makes Sobol
// worth using, the GPU's flat path draws the generic engine's points under
// it, and it prices without bias.

namespace quantModeling
{
    // Output bit k (from the top) depends only on the input's top k bits,
    // and each level permutes its intervals: a nested permutation.
    TEST(SobolOwen, ScramblingIsANestedPermutation)
    {
        std::mt19937 gen(5);
        for (int trial = 0; trial < 20; ++trial)
        {
            const uint32_t seed = static_cast<uint32_t>(gen());
            for (int k = 1; k <= 12; ++k)
            {
                const uint32_t top = ~0u << (32 - k);
                // same top-k bits in -> same top-k bits out
                for (int i = 0; i < 50; ++i)
                {
                    const uint32_t x = static_cast<uint32_t>(gen());
                    const uint32_t y = (x & top) | (static_cast<uint32_t>(gen()) & ~top);
                    ASSERT_EQ(owen_scramble(x, seed) & top, owen_scramble(y, seed) & top) << k;
                }
                // the 2^k prefixes are permuted
                std::set<uint32_t> seen;
                for (uint32_t i = 0; i < (1u << k); ++i)
                    seen.insert(owen_scramble(i << (32 - k), seed) & top);
                ASSERT_EQ(seen.size(), static_cast<std::size_t>(1u) << k) << k;
            }
        }
    }

    // The first 2^m points of a scrambled dimension still put exactly one
    // point in each interval [j 2^-m, (j + 1) 2^-m): a (0, m, 1)-net.
    TEST(SobolOwen, ScrambledPointsStayANet)
    {
        for (int d : {1, 2, 7, 100})
        {
            const int m = 12;
            SobolSequence seq(d, 42, /*owen=*/true);
            std::vector<double> u(static_cast<std::size_t>(d));
            std::vector<int> hits(1u << m, 0);
            for (int p = 0; p < (1 << m); ++p)
            {
                seq.next_uniform(u);
                ++hits[static_cast<std::size_t>(u.back() * (1 << m))];
            }
            for (int h : hits)
                ASSERT_EQ(h, 1) << "dimension " << d;
        }
    }

    // The GPU's flat point + bridge draws the generic engine's points, Owen
    // or not, bit for bit.
    TEST(SobolOwen, FlatPointEqualsTheGenericEngines)
    {
        const std::vector<Time> times{0.1, 0.2, 0.35, 0.5, 0.8, 1.0, 1.4};
        const std::size_t dim = times.size() * 2;
        const BrownianLayout layout{times, 2, 2};
        for (const bool owen : {false, true})
        {
            const mc::SobolTables t = mc::sobol_tables(dim, 42, 2, layout, true, owen);
            const mc::SobolBridgeView v = t.view();
            std::vector<double> pt(dim), out(dim), point(dim), ref(dim);
            for (int b = 0; b < 2; ++b)
            {
                SobolSequence seq(static_cast<int>(dim), (static_cast<uint64_t>(42u) << 32) | static_cast<uint64_t>(b),
                                  owen);
                BridgedGaussians bridge(times, 2, 2);
                for (uint32_t p = 0; p < 300; ++p)
                {
                    seq.next_gaussian(point);
                    bridge.map(point, ref);
                    mc::sobol_bridged_gaussians(v, static_cast<uint64_t>(b), p, pt.data(), 1, out.data(), 1);
                    for (std::size_t j = 0; j < dim; ++j)
                        ASSERT_EQ(out[j], ref[j]) << (owen ? "owen " : "shift ") << b << " " << p << " " << j;
                }
            }
        }
    }

    // The scrambling with a coin per node (owen_scramble_nested, #226): a
    // nested permutation too, a net, and -- what the hash only approximates --
    // the coins of two sibling nodes independent across seeds.
    TEST(SobolOwen, NestedScramblingIsANestedPermutation)
    {
        std::mt19937 gen(9);
        for (int trial = 0; trial < 20; ++trial)
        {
            const uint32_t seed = static_cast<uint32_t>(gen());
            for (int k = 1; k <= 12; ++k)
            {
                const uint32_t top = ~0u << (32 - k);
                for (int i = 0; i < 50; ++i)
                {
                    const uint32_t x = static_cast<uint32_t>(gen());
                    const uint32_t y = (x & top) | (static_cast<uint32_t>(gen()) & ~top);
                    ASSERT_EQ(owen_scramble_nested(x, seed) & top, owen_scramble_nested(y, seed) & top) << k;
                }
                std::set<uint32_t> seen;
                for (uint32_t i = 0; i < (1u << k); ++i)
                    seen.insert(owen_scramble_nested(i << (32 - k), seed) & top);
                ASSERT_EQ(seen.size(), static_cast<std::size_t>(1u) << k) << k;
            }
        }
        for (int d : {1, 7, 100})
        {
            const int m = 12;
            SobolSequence seq(d, 42, /*owen=*/true, /*owen_nested=*/true);
            std::vector<double> u(static_cast<std::size_t>(d));
            std::vector<int> hits(1u << m, 0);
            for (int p = 0; p < (1 << m); ++p)
            {
                seq.next_uniform(u);
                ++hits[static_cast<std::size_t>(u.back() * (1 << m))];
            }
            for (int h : hits)
                ASSERT_EQ(h, 1) << "dimension " << d;
        }
    }

    TEST(SobolOwen, NestedCoinsAreFairAndIndependentBetweenSiblings)
    {
        // At level k, the flips of the 2^k nodes, read over many seeds: each
        // is a fair coin, and two siblings agree half the time.
        const int seeds = 20000;
        for (int k : {1, 3, 8, 20})
        {
            int ones = 0, agree = 0;
            const uint32_t a = 0x2468ace1u, b = a ^ (1u << (32 - k)); // differ in bit k-1 only
            for (int s = 0; s < seeds; ++s)
            {
                const uint32_t seed = static_cast<uint32_t>(s) * 2654435761u;
                const uint32_t bit = 1u << (31 - k);
                const uint32_t fa = (owen_scramble_nested(a, seed) ^ a) & bit;
                const uint32_t fb = (owen_scramble_nested(b, seed) ^ b) & bit;
                ones += fa != 0;
                agree += (fa != 0) == (fb != 0);
            }
            const double sd = 0.5 / std::sqrt(static_cast<double>(seeds));
            EXPECT_NEAR(ones / static_cast<double>(seeds), 0.5, 5 * sd) << "level " << k;
            EXPECT_NEAR(agree / static_cast<double>(seeds), 0.5, 5 * sd) << "level " << k;
        }
    }

    TEST(SobolOwen, NestedFlatPointEqualsTheGenericEngines)
    {
        const std::vector<Time> times{0.1, 0.2, 0.35, 0.5, 0.8, 1.0, 1.4};
        const std::size_t dim = times.size();
        const BrownianLayout layout{times, 1, 1};
        const mc::SobolTables t = mc::sobol_tables(dim, 42, 2, layout, true, true, true);
        const mc::SobolBridgeView v = t.view();
        std::vector<double> pt(dim), out(dim), point(dim), ref(dim);
        for (int b = 0; b < 2; ++b)
        {
            SobolSequence seq(static_cast<int>(dim), (static_cast<uint64_t>(42u) << 32) | static_cast<uint64_t>(b),
                              true, true);
            BridgedGaussians bridge(times, 1, 1);
            for (uint32_t p = 0; p < 300; ++p)
            {
                seq.next_gaussian(point);
                bridge.map(point, ref);
                mc::sobol_bridged_gaussians(v, static_cast<uint64_t>(b), p, pt.data(), 1, out.data(), 1);
                for (std::size_t j = 0; j < dim; ++j)
                    ASSERT_EQ(out[j], ref[j]) << b << " " << p << " " << j;
            }
        }
    }

    TEST(SobolOwen, PricesACallWithoutBias)
    {
        const ValuationContext ctx{Date::from_iso("2026-06-01")};
        const ScriptedProduct<Real> product("2027-06-01\n    pays max(spot() - 100, 0)\n", ctx);
        const double T = ctx.t(Date::from_iso("2027-06-01"));
        const double S = 100, K = 100, r = 0.03, q = 0.01, s = 0.25;
        const double d1 = (std::log(S / K) + (r - q + 0.5 * s * s) * T) / (s * std::sqrt(T));
        const double exact = S * std::exp(-q * T) * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d1 - s * std::sqrt(T));
        PricingSettings set;
        set.mc_paths = 1 << 16;
        set.mc_sampler = SamplerKind::Sobol;
        set.mc_sobol_owen = true;
        for (const bool nested : {false, true})
        {
            set.mc_sobol_owen_nested = nested;
            BlackScholesSimModel<Real> m(S, r, q, s);
            const SimulationMCResult res = simulate<Real>(product, m, set);
            EXPECT_NE(res.diagnostics.find("Owen"), std::string::npos) << res.diagnostics;
            EXPECT_NEAR(res.npv(), exact, 4.0 * res.std_error() + 1e-12) << "nested " << nested;
            EXPECT_LT(res.std_error(), 2e-3);
        }
    }

} // namespace quantModeling
