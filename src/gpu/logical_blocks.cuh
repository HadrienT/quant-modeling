#ifndef GPU_LOGICAL_BLOCKS_CUH
#define GPU_LOGICAL_BLOCKS_CUH

// Device side of engines/mc/logical_blocks.hpp: the same reduction tree,
// executed by one CUDA block per logical block. Keep the two in step -- the
// host version is what the CPU runs and what the GPU is tested against.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/gpu/device.hpp"

namespace quantModeling::gpu::detail
{

    /// Refuse a device index this process cannot use, in words a user of the
    /// site can act on.
    inline void require_device(int device)
    {
        const int n = device_count();
        if (n == 0)
            throw GpuUnavailable("GPU requested, but this server has no usable CUDA device");
        if (device < 0 || device >= n)
            throw GpuUnavailable("GPU " + std::to_string(device) + " requested, but this server has " +
                                 std::to_string(n) + " usable CUDA device(s)");
    }

    inline void check(cudaError_t err, const char *what)
    {
        if (err != cudaSuccess)
            throw GpuUnavailable(std::string("CUDA error in ") + what + ": " + cudaGetErrorString(err));
    }

    /**
     * @brief Bytes of freed device memory the current device's pool keeps
     *        for the next request: QM_GPU_POOL_RETAIN_MB, 256 MiB by default.
     *
     * The cards are shared with the scripting assistant's LLM: what the pool
     * keeps is not free for it. 256 MiB holds a small or medium pricing's
     * every buffer; a large one allocates beyond it and gives the excess back
     * when it ends.
     */
    inline std::size_t pool_retain_bytes()
    {
        static const std::size_t bytes = []
        {
            std::size_t mb = 256;
            if (const char *env = std::getenv("QM_GPU_POOL_RETAIN_MB"))
                mb = static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
            return mb << 20;
        }();
        return bytes;
    }

    /// The current device's default memory pool, its release threshold set
    /// on first use.
    inline cudaMemPool_t current_pool()
    {
        int device = 0;
        check(cudaGetDevice(&device), "cudaGetDevice");
        static std::once_flag configured[64];
        cudaMemPool_t pool = nullptr;
        check(cudaDeviceGetDefaultMemPool(&pool, device), "cudaDeviceGetDefaultMemPool");
        if (device >= 0 && device < 64)
            std::call_once(configured[device],
                           [pool]
                           {
                               uint64_t threshold = pool_retain_bytes();
                               cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &threshold);
                           });
        return pool;
    }

    /**
     * @brief Device memory from the current device's stream-ordered pool
     *        (cudaMallocAsync on the legacy default stream, the one every
     *        kernel and copy here runs on); give it back with device_free.
     *
     * A request used to cudaMalloc and cudaFree ~30 buffers: 430 us of a
     * 1.7 ms call whose kernels ran 21 us (one V100, nvprof, 2026-11-19;
     * issue #330). The pool reuses what the last request freed. Out of
     * memory, the pool's idle memory is released and the allocation retried
     * once before the error reaches the user.
     */
    template <class T>
    T *device_alloc(std::size_t n, const char *what = "cudaMallocAsync")
    {
        void *p = nullptr;
        if (n == 0)
            return nullptr;
        const std::size_t bytes = n * sizeof(T);
        cudaError_t err = cudaMallocAsync(&p, bytes, current_pool(), 0);
        if (err == cudaErrorMemoryAllocation)
        {
            cudaGetLastError();
            check(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
            cudaMemPoolTrimTo(current_pool(), 0);
            err = cudaMallocAsync(&p, bytes, current_pool(), 0);
        }
        check(err, what);
        return static_cast<T *>(p);
    }

    /**
     * @brief The most memory one launch may take on `device`: half of what
     *        is free (the cards are shared with the assistant's LLM) -- not
     *        queried for a request needing at most 64 MiB in all.
     *
     * Below that, half of the free memory could only cap a launch on a card
     * already nearly full, where the allocation reaches device_alloc's
     * out-of-memory path anyway; the two cudaMemGetInfo of a small request
     * cost 44 us of its 0.41 ms (one V100, nvprof, 2026-11-19; issue #330).
     * The result never depends on the launch size.
     */
    inline std::size_t launch_budget(int device, std::size_t need_bytes)
    {
        constexpr std::size_t kSmallRequestBytes = std::size_t{64} << 20;
        return need_bytes <= kSmallRequestBytes ? need_bytes : free_memory(device) / 2;
    }

    /// Returns device_alloc's memory to its pool, after the work queued
    /// before it on the default stream.
    inline void device_free(void *p) noexcept
    {
        if (p)
            cudaFreeAsync(p, 0);
    }

    /**
     * @brief Gives back, on scope exit, the local memory a kernel's stack
     *        made the driver reserve.
     *
     * The driver sizes a launch's local memory as the kernel's stack frame
     * times every thread the device can hold (2 048 per SM x 80 SMs on a
     * V100) and keeps it after the launch: 13 KB of dual numbers per thread
     * holds ~2 GB of a card the scripting assistant's LLM shares. Setting the
     * stack limit back to its value before the run releases it (measured:
     * 1 952 MB back); the next launch grows it again as needed.
     */
    struct StackLimitRestore
    {
        std::size_t before = 0;
        bool ok = false;
        StackLimitRestore() { ok = cudaDeviceGetLimit(&before, cudaLimitStackSize) == cudaSuccess; }
        ~StackLimitRestore()
        {
            if (ok)
                cudaDeviceSetLimit(cudaLimitStackSize, before);
        }
        StackLimitRestore(const StackLimitRestore &) = delete;
        StackLimitRestore &operator=(const StackLimitRestore &) = delete;
    };

    /// __shfl_down_sync of a trivially copyable struct, word by word.
    template <class T>
    __device__ T shfl_down(const T &x, unsigned offset)
    {
        static_assert(sizeof(T) % sizeof(int) == 0, "shuffled type must be a whole number of words");
        constexpr int kWords = sizeof(T) / sizeof(int);
        int in[kWords];
        int out[kWords];
        memcpy(in, &x, sizeof(T));
        for (int i = 0; i < kWords; ++i)
            out[i] = __shfl_down_sync(0xffffffffu, in[i], offset);
        T r;
        memcpy(&r, out, sizeof(T));
        return r;
    }

    /// fn(segment, u) when the functor takes a segment, fn(u) otherwise.
    template <class UnitFn>
    __device__ auto call_unit(const UnitFn &fn, uint64_t segment, uint64_t u)
    {
        if constexpr (std::is_invocable_v<const UnitFn &, uint64_t, uint64_t>)
            return fn(segment, u);
        else
            return fn(u);
    }

    /**
     * One CUDA block per logical block. With several segments (independent
     * replicates of n_units units each, lot G3), grid block g is logical
     * block g % blocks_per_segment of segment g / blocks_per_segment: every
     * segment is reduced by exactly the tree of a launch of its own, and all
     * of them fill the device at once.
     */
    /**
     * The reduction of one logical block (blockIdx.x of the launch), the
     * order that fixes the bits: thread t adds the samples of units t,
     * t + 256, ... in that order (`sample(segment, u, l)`, l = u's index in
     * the block), then the warp tree, then the warps in order.
     */
    template <class Stats, class Sample>
    __device__ __forceinline__ void logical_block_reduce(uint64_t first_block, uint64_t blocks_per_segment,
                                                         uint64_t n_units, const Sample &sample, Stats *partials)
    {
        using L = mc::LogicalBlocks;
        const uint64_t g = first_block + blockIdx.x;
        const uint64_t segment = g / blocks_per_segment;
        const uint64_t base = (g % blocks_per_segment) * L::kUnitsPerBlock;
        const unsigned t = threadIdx.x;

        Stats acc;
        for (int k = 0; k < L::kUnitsPerThread; ++k)
        {
            const unsigned l = t + static_cast<unsigned>(k) * L::kThreadsPerBlock;
            const uint64_t u = base + l;
            if (u < n_units)
                acc.add(sample(segment, u, l));
        }

        // Warp: lane i merges lane i + offset, offsets 16..1.
        for (unsigned offset = L::kWarpSize / 2; offset > 0; offset /= 2)
        {
            const Stats other = shfl_down(acc, offset);
            acc.merge(other);
        }

        // Block: warp results folded in warp order. Raw storage because a
        // __shared__ variable cannot have a constructor.
        __shared__ alignas(Stats) unsigned char smem[sizeof(Stats) * L::kWarpsPerBlock];
        Stats *warps = reinterpret_cast<Stats *>(smem);
        const unsigned lane = t % L::kWarpSize;
        const unsigned warp = t / L::kWarpSize;
        if (lane == 0)
            memcpy(&warps[warp], &acc, sizeof(Stats));
        __syncthreads();
        if (t == 0)
        {
            Stats out;
            memcpy(&out, &warps[0], sizeof(Stats));
            for (int w = 1; w < L::kWarpsPerBlock; ++w)
            {
                Stats next;
                memcpy(&next, &warps[w], sizeof(Stats));
                out.merge(next);
            }
            partials[blockIdx.x] = out;
        }
    }

    /// One logical block: each thread runs its 8 units one after the other
    /// and reduces them (the units computed where they are added).
    template <class Stats, class UnitFn>
    __device__ __forceinline__ void logical_block_body(uint64_t first_block, uint64_t blocks_per_segment,
                                                       uint64_t n_units, const UnitFn &fn, Stats *partials)
    {
        logical_block_reduce<Stats>(
            first_block, blocks_per_segment, n_units,
            [&](uint64_t segment, uint64_t u, unsigned)
            { return call_unit(fn, segment, u); }, partials);
    }

    /**
     * Parallel units (lot P1g of blueprint/wp/26-performance.md): one thread
     * per unit of the launch's logical blocks, whatever the grid -- thread i
     * computes unit l = i % kUnitsPerBlock of logical block i / kUnitsPerBlock and stores its
     * sample at samples[i]. logical_block_reduce then adds the stored samples
     * in the order above: the same additions, the same bits, sixteen times
     * the threads in flight.
     */
    template <class UnitFn, class Out>
    __device__ __forceinline__ void logical_block_units(uint64_t first_block, uint64_t blocks_per_segment,
                                                        uint64_t n_units, uint64_t n_blocks, const UnitFn &fn,
                                                        Out *samples)
    {
        using L = mc::LogicalBlocks;
        const uint64_t i = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
        if (i >= n_blocks * L::kUnitsPerBlock)
            return;
        const uint64_t g = first_block + i / L::kUnitsPerBlock;
        const uint64_t u = (g % blocks_per_segment) * L::kUnitsPerBlock + i % L::kUnitsPerBlock;
        if (u < n_units)
            samples[i] = call_unit(fn, g / blocks_per_segment, u);
    }

    template <class UnitFn, class Out>
    __global__ void __launch_bounds__(mc::LogicalBlocks::kThreadsPerBlock)
        logical_units_kernel(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                             UnitFn fn, Out *samples)
    {
        logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, fn, samples);
    }

    /// The reduction of stored samples, one CUDA block per logical block.
    template <class Stats, class Out>
    __global__ void __launch_bounds__(mc::LogicalBlocks::kThreadsPerBlock)
        logical_reduce_kernel(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units,
                              const Out *samples, Stats *partials)
    {
        const Out *block = samples + static_cast<uint64_t>(blockIdx.x) * mc::LogicalBlocks::kUnitsPerBlock;
        logical_block_reduce<Stats>(
            first_block, blocks_per_segment, n_units, [&](uint64_t, uint64_t, unsigned l)
            { return block[l]; },
            partials);
    }

    /// The kernel of logical_block_body. The GPU's compiled scripts (lot
    /// P1b of blueprint/wp/26-performance.md) launch the same body from a
    /// kernel linked at run time (script_jit_kernel.cu).
    template <class Stats, class UnitFn>
    __global__ void __launch_bounds__(mc::LogicalBlocks::kThreadsPerBlock)
        logical_block_kernel(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, UnitFn fn,
                             Stats *partials)
    {
        logical_block_body<Stats>(first_block, blocks_per_segment, n_units, fn, partials);
    }

    /// Called after each launch with its first logical block and block count,
    /// before the next launch reuses any per-thread scratch.
    using AfterLaunch = std::function<void(uint64_t first_block, uint64_t count)>;

    /// Launches `count` logical blocks from `first_block` (a global index),
    /// their partials to `d_partials`.
    template <class Stats>
    using LaunchBlocks = std::function<void(uint64_t first_block, uint64_t count, Stats *d_partials)>;

    /**
     * @brief The partials of logical blocks [first, last) -- global indices
     *        over `blocks_per_segment`-block segments of n_units units -- on
     *        `device`, in block order, in as few launches as memory allows.
     */
    template <class Stats>
    std::vector<Stats> run_block_range_with(int device, uint64_t first, uint64_t last,
                                            uint64_t max_blocks_per_launch, const LaunchBlocks<Stats> &launch,
                                            const AfterLaunch &after_launch = {})
    {
        require_device(device);
        check(cudaSetDevice(device), "cudaSetDevice");
        std::vector<Stats> partials(static_cast<size_t>(last > first ? last - first : 0));
        if (last <= first)
            return partials;

        // Only the partials live on the device; take at most half of what is
        // free (the assistant's LLM shares the card) and cap one launch so a
        // single kernel stays short.
        const std::size_t budget = launch_budget(device, (last - first) * sizeof(Stats));
        uint64_t per_launch = std::max<uint64_t>(1, budget / sizeof(Stats));
        per_launch = std::min<uint64_t>(per_launch, uint64_t{1} << 20);
        if (max_blocks_per_launch > 0)
            per_launch = std::min(per_launch, max_blocks_per_launch);
        per_launch = std::min(per_launch, last - first);

        Stats *d_partials = device_alloc<Stats>(per_launch);
        try
        {
            for (uint64_t b = first; b < last; b += per_launch)
            {
                const uint64_t count = std::min(per_launch, last - b);
                launch(b, count, d_partials);
                check(cudaMemcpy(&partials[static_cast<size_t>(b - first)], d_partials, count * sizeof(Stats),
                                 cudaMemcpyDeviceToHost),
                      "cudaMemcpy");
                if (after_launch)
                    after_launch(b, count);
            }
        }
        catch (...)
        {
            device_free(d_partials);
            throw;
        }
        device_free(d_partials);
        return partials;
    }

    /// Launches the units of `count` logical blocks from `first_block` (one
    /// thread per unit, logical_block_units), their samples to `d_samples`.
    template <class Out>
    using LaunchUnits = std::function<void(uint64_t first_block, uint64_t count, Out *d_samples)>;

    /// The CUDA blocks of 256 threads that cover `count` logical blocks' units.
    inline unsigned unit_grid(uint64_t count)
    {
        return static_cast<unsigned>(count * mc::LogicalBlocks::kUnitsPerThread);
    }

    /**
     * @brief run_block_range_with, the units in parallel (lot P1g): per
     *        launch, `launch_units` computes every unit's sample, then
     *        logical_reduce_kernel reduces them in logical-block order --
     *        the partials, bit for bit, of the serial kernel.
     *
     * The launch size comes from free memory (the samples and partials, at
     * most half of what is free) and from `max_blocks_per_launch` (the
     * caller's per-unit scratch). `after_launch` runs after each launch's
     * reduction, before the next launch reuses the per-unit scratch.
     */
    template <class Stats, class Out>
    std::vector<Stats> run_block_range_units(int device, uint64_t first, uint64_t last, uint64_t blocks_per_segment,
                                             uint64_t n_units, uint64_t max_blocks_per_launch,
                                             const LaunchUnits<Out> &launch_units,
                                             const AfterLaunch &after_launch = {})
    {
        using L = mc::LogicalBlocks;
        require_device(device);
        check(cudaSetDevice(device), "cudaSetDevice");
        std::vector<Stats> partials(static_cast<size_t>(last > first ? last - first : 0));
        if (last <= first)
            return partials;

        const std::size_t per_block = sizeof(Stats) + static_cast<std::size_t>(L::kUnitsPerBlock) * sizeof(Out);
        const std::size_t budget = launch_budget(device, (last - first) * per_block);
        uint64_t per_launch = std::max<uint64_t>(1, budget / per_block);
        per_launch = std::min<uint64_t>(per_launch, uint64_t{1} << 16); // a grid of at most 2^20 CUDA blocks
        if (max_blocks_per_launch > 0)
            per_launch = std::min(per_launch, max_blocks_per_launch);
        per_launch = std::min(per_launch, last - first);

        Stats *d_partials = device_alloc<Stats>(per_launch);
        Out *d_samples = nullptr;
        try
        {
            d_samples = device_alloc<Out>(per_launch * L::kUnitsPerBlock);
        }
        catch (...)
        {
            device_free(d_partials);
            throw;
        }
        try
        {
            for (uint64_t b = first; b < last; b += per_launch)
            {
                const uint64_t count = std::min(per_launch, last - b);
                launch_units(b, count, d_samples);
                logical_reduce_kernel<Stats, Out><<<static_cast<unsigned>(count), L::kThreadsPerBlock>>>(
                    b, blocks_per_segment, n_units, d_samples, d_partials);
                check(cudaGetLastError(), "reduce launch");
                check(cudaMemcpy(&partials[static_cast<size_t>(b - first)], d_partials, count * sizeof(Stats),
                                 cudaMemcpyDeviceToHost),
                      "cudaMemcpy");
                if (after_launch)
                    after_launch(b, count);
            }
        }
        catch (...)
        {
            device_free(d_samples);
            device_free(d_partials);
            throw;
        }
        device_free(d_samples);
        device_free(d_partials);
        return partials;
    }

    /// The sample type of a unit functor: its `Out` (declared, not deduced
    /// from the __device__ call, which nvcc's host and device passes may see
    /// differently -- the kernel would then not exist on the device).
    template <class UnitFn>
    using UnitSample = typename UnitFn::Out;

    /// run_block_range_units on an nvcc-compiled functor.
    template <class Stats, class UnitFn>
    std::vector<Stats> run_block_range_parallel(int device, uint64_t first, uint64_t last,
                                                uint64_t blocks_per_segment, uint64_t n_units, const UnitFn &fn,
                                                uint64_t max_blocks_per_launch)
    {
        using Out = UnitSample<UnitFn>;
        const LaunchUnits<Out> launch = [&](uint64_t b, uint64_t count, Out *d_samples)
        {
            logical_units_kernel<UnitFn, Out><<<unit_grid(count), mc::LogicalBlocks::kThreadsPerBlock>>>(
                b, blocks_per_segment, n_units, count, fn, d_samples);
            check(cudaGetLastError(), "units launch");
        };
        return run_block_range_units<Stats, Out>(device, first, last, blocks_per_segment, n_units,
                                                 max_blocks_per_launch, launch);
    }

    /// run_block_range_with, launching logical_block_kernel on `fn`.
    template <class Stats, class UnitFn>
    std::vector<Stats> run_block_range(int device, uint64_t first, uint64_t last, uint64_t blocks_per_segment,
                                       uint64_t n_units, const UnitFn &fn, uint64_t max_blocks_per_launch,
                                       const AfterLaunch &after_launch = {})
    {
        const LaunchBlocks<Stats> launch = [&](uint64_t b, uint64_t count, Stats *d_partials)
        {
            logical_block_kernel<Stats, UnitFn><<<static_cast<unsigned>(count), mc::LogicalBlocks::kThreadsPerBlock>>>(
                b, blocks_per_segment, n_units, fn, d_partials);
            check(cudaGetLastError(), "kernel launch");
        };
        return run_block_range_with<Stats>(device, first, last, max_blocks_per_launch, launch, after_launch);
    }

    /// Fold each segment's partials (blocks_per_segment consecutive ones) in
    /// block order.
    template <class Stats>
    std::vector<Stats> merge_segments(const std::vector<Stats> &partials, uint64_t n_segments,
                                      uint64_t blocks_per_segment)
    {
        std::vector<Stats> out(static_cast<size_t>(n_segments));
        for (uint64_t sg = 0; sg < n_segments && blocks_per_segment > 0; ++sg)
        {
            const auto b = partials.begin() + static_cast<std::ptrdiff_t>(sg * blocks_per_segment);
            out[static_cast<size_t>(sg)] =
                mc::merge_in_order(std::vector<Stats>(b, b + static_cast<std::ptrdiff_t>(blocks_per_segment)));
        }
        return out;
    }

    /**
     * @brief Run `n_segments` segments of n_units units each on `device`, as
     *        few launches as free memory allows; fold each segment's
     *        partials in block order. The functor is fn(segment, u) or fn(u).
     */
    template <class Stats, class UnitFn>
    std::vector<Stats> run_logical_block_segments(int device, uint64_t n_segments, uint64_t n_units,
                                                  const UnitFn &fn, uint64_t max_blocks_per_launch,
                                                  const AfterLaunch &after_launch = {})
    {
        const uint64_t per_segment = mc::LogicalBlocks::count(n_units);
        return merge_segments(run_block_range<Stats>(device, 0, per_segment * n_segments, per_segment, n_units, fn,
                                                     max_blocks_per_launch, after_launch),
                              n_segments, per_segment);
    }

    /**
     * @brief Run all logical blocks of n_units on `device`, as few launches as
     *        free memory allows, and fold the partials in block order.
     */
    template <class Stats, class UnitFn>
    Stats run_logical_blocks(int device, uint64_t n_units, const UnitFn &fn, uint64_t max_blocks_per_launch,
                             const AfterLaunch &after_launch = {})
    {
        return run_logical_block_segments<Stats>(device, 1, n_units, fn, max_blocks_per_launch, after_launch)
            .front();
    }

    /**
     * @brief Share n_blocks logical blocks between `devices` (blueprint §7,
     *        lot G4): device k gets the k-th contiguous range, runs it on its
     *        own host thread (job(device, first, last) returns its partials
     *        in block order), and the ranges are concatenated in block order.
     *
     * Every block is reduced by the same tree whichever card runs it, and the
     * host folds the partials in block order: the result does not depend on
     * the number of devices -- bit for bit (ADR-G5).
     */
    template <class Stats, class Job>
    std::vector<Stats> run_on_devices(const std::vector<int> &devices, uint64_t n_blocks, const Job &job)
    {
        const std::size_t n = devices_used(devices.size(), n_blocks);
        if (n == 1)
            return job(devices.front(), 0, n_blocks);
        std::vector<std::vector<Stats>> parts(n);
        std::vector<std::exception_ptr> errors(n);
        std::vector<std::thread> threads;
        for (std::size_t k = 0; k < n; ++k)
        {
            const uint64_t first = n_blocks * k / n, last = n_blocks * (k + 1) / n;
            threads.emplace_back(
                [&, k, first, last]()
                {
                    try
                    {
                        parts[k] = job(devices[k], first, last);
                    }
                    catch (...)
                    {
                        errors[k] = std::current_exception();
                    }
                });
        }
        for (std::thread &t : threads)
            t.join();
        for (const std::exception_ptr &e : errors)
            if (e)
                std::rethrow_exception(e);
        std::vector<Stats> all;
        all.reserve(static_cast<size_t>(n_blocks));
        for (auto &p : parts)
            all.insert(all.end(), p.begin(), p.end());
        return all;
    }

} // namespace quantModeling::gpu::detail

#endif // GPU_LOGICAL_BLOCKS_CUH
