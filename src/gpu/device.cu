#include "quantModeling/gpu/device.hpp"
#include "logical_blocks.cuh"

#include <cuda_runtime.h>

namespace quantModeling::gpu
{

    bool compiled_with_cuda() noexcept
    {
        return true;
    }

    int device_count() noexcept
    {
        // Probed once: cudaGetDeviceCount initializes the driver, and the
        // answer does not change during the process's life.
        static const int count = []
        {
            int n = 0;
            if (cudaGetDeviceCount(&n) != cudaSuccess)
            {
                cudaGetLastError(); // clear the sticky "no device" error
                return 0;
            }
            return n;
        }();
        return count;
    }

    std::string device_name(int device)
    {
        // Read once per device: cudaGetDeviceProperties fills every property
        // and cost 127 us a call, paid by every GPU pricing for its
        // diagnostics label (one V100, nvprof, 2026-11-19; issue #330).
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> out(static_cast<std::size_t>(device_count()));
            for (int d = 0; d < device_count(); ++d)
            {
                cudaDeviceProp prop{};
                if (cudaGetDeviceProperties(&prop, d) == cudaSuccess)
                    out[static_cast<std::size_t>(d)] = prop.name;
                else
                    cudaGetLastError();
            }
            return out;
        }();
        if (device < 0 || device >= static_cast<int>(names.size()))
            return "";
        return names[static_cast<std::size_t>(device)];
    }

    std::size_t free_memory(int device)
    {
        if (device < 0 || device >= device_count())
            return 0;
        int previous = 0;
        cudaGetDevice(&previous);
        std::size_t free_bytes = 0, total_bytes = 0;
        if (cudaSetDevice(device) != cudaSuccess || cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess)
            free_bytes = 0;
        cudaSetDevice(previous);
        return free_bytes;
    }

} // namespace quantModeling::gpu

namespace quantModeling::gpu
{
    std::size_t trim_idle_pools(std::size_t keep_bytes)
    {
        std::size_t released = 0;
        int previous = 0;
        if (cudaGetDevice(&previous) != cudaSuccess)
        {
            cudaGetLastError();
            return 0;
        }
        for (int device = 0; device < device_count() && device < 64; ++device)
        {
            if (!detail::pools_in_use()[device].load(std::memory_order_relaxed))
                continue;
            cudaMemPool_t pool = nullptr;
            if (cudaDeviceGetDefaultMemPool(&pool, device) != cudaSuccess)
            {
                cudaGetLastError();
                continue;
            }
            uint64_t before = 0, after = 0;
            cudaMemPoolGetAttribute(pool, cudaMemPoolAttrReservedMemCurrent, &before);
            if (before <= keep_bytes)
                continue;
            // Memory given back by cudaFreeAsync is released only once a
            // synchronisation has seen the free complete: without it the trim
            // released nothing (4.76 GB stayed reserved after the local-vol
            // adjoint). Called from the API's idle watcher, never on a
            // request's path, so the wait costs no pricing anything. Memory
            // an allocation still holds is never released: a request running
            // on another thread keeps its memory.
            if (cudaSetDevice(device) != cudaSuccess || cudaDeviceSynchronize() != cudaSuccess)
            {
                cudaGetLastError();
                continue;
            }
            cudaMemPoolTrimTo(pool, keep_bytes);
            cudaMemPoolGetAttribute(pool, cudaMemPoolAttrReservedMemCurrent, &after);
            released += before > after ? static_cast<std::size_t>(before - after) : 0;
        }
        cudaGetLastError();
        cudaSetDevice(previous);
        return released;
    }
} // namespace quantModeling::gpu
