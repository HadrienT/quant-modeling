#ifndef GPU_LOGICAL_BLOCKS_CUH
#define GPU_LOGICAL_BLOCKS_CUH

// Device side of engines/mc/logical_blocks.hpp: the same reduction tree,
// executed by one CUDA block per logical block. Keep the two in step -- the
// host version is what the CPU runs and what the GPU is tested against.

#include <algorithm>
#include <cstdint>
#include <cstring>
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

    template <class Stats, class UnitFn>
    __global__ void __launch_bounds__(mc::LogicalBlocks::kThreadsPerBlock)
        logical_block_kernel(uint64_t first_block, uint64_t n_units, UnitFn fn, Stats *partials)
    {
        using L = mc::LogicalBlocks;
        const uint64_t base = (first_block + blockIdx.x) * L::kUnitsPerBlock;
        const unsigned t = threadIdx.x;

        Stats acc;
        for (int k = 0; k < L::kUnitsPerThread; ++k)
        {
            const uint64_t u = base + t + static_cast<uint64_t>(k) * L::kThreadsPerBlock;
            if (u < n_units)
                acc.add(fn(u));
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

    /**
     * @brief Run all logical blocks of n_units on `device`, as few launches as
     *        free memory allows, and fold the partials in block order.
     */
    template <class Stats, class UnitFn>
    Stats run_logical_blocks(int device, uint64_t n_units, const UnitFn &fn, uint64_t max_blocks_per_launch)
    {
        require_device(device);
        check(cudaSetDevice(device), "cudaSetDevice");

        const uint64_t n_blocks = mc::LogicalBlocks::count(n_units);
        if (n_blocks == 0)
            return Stats{};

        // Only the partials live on the device; take at most half of what is
        // free (the assistant's LLM shares the card) and cap one launch so a
        // single kernel stays short.
        uint64_t per_launch = std::max<uint64_t>(1, free_memory(device) / 2 / sizeof(Stats));
        per_launch = std::min<uint64_t>(per_launch, uint64_t{1} << 20);
        if (max_blocks_per_launch > 0)
            per_launch = std::min(per_launch, max_blocks_per_launch);
        per_launch = std::min(per_launch, n_blocks);

        Stats *d_partials = nullptr;
        check(cudaMalloc(&d_partials, per_launch * sizeof(Stats)), "cudaMalloc");
        std::vector<Stats> partials(static_cast<size_t>(n_blocks));
        try
        {
            for (uint64_t first = 0; first < n_blocks; first += per_launch)
            {
                const uint64_t count = std::min(per_launch, n_blocks - first);
                logical_block_kernel<Stats, UnitFn>
                    <<<static_cast<unsigned>(count), mc::LogicalBlocks::kThreadsPerBlock>>>(first, n_units, fn, d_partials);
                check(cudaGetLastError(), "kernel launch");
                check(cudaMemcpy(&partials[static_cast<size_t>(first)], d_partials, count * sizeof(Stats),
                                 cudaMemcpyDeviceToHost),
                      "cudaMemcpy");
            }
        }
        catch (...)
        {
            cudaFree(d_partials);
            throw;
        }
        check(cudaFree(d_partials), "cudaFree");
        return mc::merge_in_order(partials);
    }

} // namespace quantModeling::gpu::detail

#endif // GPU_LOGICAL_BLOCKS_CUH
