#include "quantModeling/gpu/rng.hpp"
#include "quantModeling/utils/philox.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        __global__ void philox_kernel(uint64_t seed, uint64_t first_path, uint32_t n_paths, uint32_t draws,
                                      double *out)
        {
            const uint64_t i = blockIdx.x * static_cast<uint64_t>(blockDim.x) + threadIdx.x;
            if (i >= static_cast<uint64_t>(n_paths) * draws)
                return;
            const uint64_t p = i / draws;
            const auto j = static_cast<uint32_t>(i % draws);
            out[i] = philox_uniform(seed, first_path + p, j);
        }
    } // namespace

    std::vector<double> philox_uniforms(uint64_t seed, uint64_t first_path, uint32_t n_paths, uint32_t draws,
                                        int device)
    {
        if (device < 0 || device >= device_count())
            throw GpuUnavailable("no CUDA device " + std::to_string(device));
        detail::check(cudaSetDevice(device), "cudaSetDevice");
        const std::size_t n = static_cast<std::size_t>(n_paths) * draws;
        std::vector<double> host(n);
        if (n == 0)
            return host;
        double *d = nullptr;
        detail::check(cudaMalloc(&d, n * sizeof(double)), "cudaMalloc");
        const unsigned threads = 256;
        const auto blocks = static_cast<unsigned>((n + threads - 1) / threads);
        philox_kernel<<<blocks, threads>>>(seed, first_path, n_paths, draws, d);
        const cudaError_t launch = cudaGetLastError();
        const cudaError_t copy = cudaMemcpy(host.data(), d, n * sizeof(double), cudaMemcpyDeviceToHost);
        cudaFree(d);
        detail::check(launch, "kernel launch");
        detail::check(copy, "cudaMemcpy");
        return host;
    }

} // namespace quantModeling::gpu
