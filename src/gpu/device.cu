#include "quantModeling/gpu/device.hpp"

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
        if (device < 0 || device >= device_count())
            return "";
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, device) != cudaSuccess)
            return "";
        return prop.name;
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
