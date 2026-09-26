#ifndef ENGINE_MC_LOGICAL_BLOCKS_HPP
#define ENGINE_MC_LOGICAL_BLOCKS_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/utils/thread_pool.hpp"

/**
 * @file logical_blocks.hpp
 * @brief The fixed reduction layout shared by the CPU and the GPU
 *        (blueprint/wp/19-gpu.md §5, §7, ADR-G5).
 *
 * Floating-point addition is not associative, so a Monte-Carlo mean depends
 * on the order in which paths are summed. To make that order a property of
 * the path indices rather than of the hardware, the work is cut into
 * *logical blocks* of kUnitsPerBlock consecutive units (a unit is one path,
 * or one antithetic pair), and every logical block is reduced by the same
 * tree:
 *
 *   - "thread" t of the block (0 <= t < kThreadsPerBlock) accumulates units
 *     t, t + kThreadsPerBlock, t + 2*kThreadsPerBlock, ... in that order;
 *   - each "warp" of 32 threads is folded by the shuffle-down tree
 *     (offsets 16, 8, 4, 2, 1: lane i merges lane i + offset);
 *   - the block folds its warps' results in warp order;
 *   - the host folds the block partials in block order.
 *
 * On the GPU this is literally the kernel's shape (one CUDA block per logical
 * block, src/gpu/logical_blocks.cuh). On the CPU, reduce_logical_blocks()
 * below replays the same tree, so the grouping — and with a counter-based
 * generator, every draw — is identical. The result therefore does not depend
 * on how many CPU threads, GPU launches or GPUs share the blocks.
 *
 * Every merge is Chan's parallel Welford formula, never a naive sum.
 *
 * `Stats` must be default-constructible (empty) and provide add(Values) and
 * merge(const Stats &). `UnitFn` is callable as fn(uint64_t unit) -> Values.
 */

namespace quantModeling::mc
{

    struct LogicalBlocks
    {
        static constexpr int kThreadsPerBlock = 256;
        static constexpr int kUnitsPerThread = 16;
        static constexpr int kUnitsPerBlock = kThreadsPerBlock * kUnitsPerThread; // 4 096
        static constexpr int kWarpSize = 32;
        static constexpr int kWarpsPerBlock = kThreadsPerBlock / kWarpSize;

        static constexpr uint64_t count(uint64_t n_units)
        {
            return (n_units + kUnitsPerBlock - 1) / kUnitsPerBlock;
        }
    };

    /// Reduce logical block `block` on the host, by the exact tree the GPU
    /// kernel uses.
    template <class Stats, class UnitFn>
    Stats reduce_logical_block(uint64_t block, uint64_t n_units, const UnitFn &fn)
    {
        using L = LogicalBlocks;
        const uint64_t base = block * L::kUnitsPerBlock;
        Stats warps[L::kWarpsPerBlock];
        for (int w = 0; w < L::kWarpsPerBlock; ++w)
        {
            Stats lanes[L::kWarpSize];
            for (int l = 0; l < L::kWarpSize; ++l)
            {
                const uint64_t t = static_cast<uint64_t>(w * L::kWarpSize + l);
                for (int k = 0; k < L::kUnitsPerThread; ++k)
                {
                    const uint64_t u = base + t + static_cast<uint64_t>(k) * L::kThreadsPerBlock;
                    if (u < n_units)
                        lanes[l].add(fn(u));
                }
            }
            for (int offset = L::kWarpSize / 2; offset > 0; offset /= 2)
                for (int l = 0; l < offset; ++l)
                    lanes[l].merge(lanes[l + offset]);
            warps[w] = lanes[0];
        }
        Stats out = warps[0];
        for (int w = 1; w < L::kWarpsPerBlock; ++w)
            out.merge(warps[w]);
        return out;
    }

    /// Fold per-block partials in block order (the only order the host uses).
    template <class Stats>
    Stats merge_in_order(const std::vector<Stats> &partials)
    {
        Stats total;
        for (const Stats &p : partials)
            total.merge(p);
        return total;
    }

    /**
     * @brief Reduce all n_units on the host. With a pool, logical blocks are
     *        spread over its threads; the partials are still folded in block
     *        order, so the result is bit-identical with or without the pool.
     */
    template <class Stats, class UnitFn>
    Stats reduce_logical_blocks(uint64_t n_units, const UnitFn &fn, ThreadPool *pool = nullptr)
    {
        const uint64_t n_blocks = LogicalBlocks::count(n_units);
        std::vector<Stats> partials(static_cast<size_t>(n_blocks));
        if (pool == nullptr || pool->num_threads() == 0 || n_blocks < 2)
        {
            for (uint64_t b = 0; b < n_blocks; ++b)
                partials[static_cast<size_t>(b)] = reduce_logical_block<Stats>(b, n_units, fn);
            return merge_in_order(partials);
        }

        // One task per contiguous chunk of blocks, a few chunks per thread so
        // that uneven block costs even out.
        const uint64_t n_tasks = std::min<uint64_t>(n_blocks, 4 * (pool->num_threads() + 1));
        std::vector<TaskHandle> handles;
        handles.reserve(static_cast<size_t>(n_tasks));
        for (uint64_t task = 0; task < n_tasks; ++task)
        {
            const uint64_t first = n_blocks * task / n_tasks;
            const uint64_t last = n_blocks * (task + 1) / n_tasks;
            handles.push_back(pool->spawn_task([&, first, last]()
                                               {
                for (uint64_t b = first; b < last; ++b)
                    partials[static_cast<size_t>(b)] = reduce_logical_block<Stats>(b, n_units, fn);
                return true; }));
        }
        for (auto &h : handles)
            pool->active_wait(h);
        return merge_in_order(partials);
    }

} // namespace quantModeling::mc

#endif // ENGINE_MC_LOGICAL_BLOCKS_HPP
