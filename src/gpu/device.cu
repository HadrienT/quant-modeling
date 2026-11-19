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
