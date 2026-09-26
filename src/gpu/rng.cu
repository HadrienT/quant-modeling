#include "quantModeling/gpu/rng.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/sobol.hpp"

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
        __global__ void sobol_kernel(const uint32_t *V, const uint32_t *shift, int dim, uint32_t first_point,
                                     uint32_t n_points, double *out)
        {
            const uint64_t i = blockIdx.x * static_cast<uint64_t>(blockDim.x) + threadIdx.x;
            if (i >= static_cast<uint64_t>(n_points) * static_cast<uint64_t>(dim))
                return;
            const auto p = static_cast<uint32_t>(i / static_cast<uint64_t>(dim));
            const auto d = static_cast<int>(i % static_cast<uint64_t>(dim));
            out[i] = sobol_to_uniform(sobol_point_bits(V + 32 * d, first_point + p), shift[d]);
        }
    } // namespace

    std::vector<double> sobol_uniforms(int dim, uint64_t scramble_seed, uint32_t first_point, uint32_t n_points,
                                       int device)
    {
        detail::require_device(device);
        detail::check(cudaSetDevice(device), "cudaSetDevice");
        const SobolSequence seq(dim, scramble_seed); // validates dim
        const auto V = seq.directions();
        const auto shift = seq.shifts();
        const std::size_t n = static_cast<std::size_t>(n_points) * static_cast<std::size_t>(dim);
        std::vector<double> host(n);
        if (n == 0)
            return host;

        uint32_t *d_V = nullptr, *d_shift = nullptr;
        double *d_out = nullptr;
        detail::check(cudaMalloc(&d_V, V.size_bytes()), "cudaMalloc");
        detail::check(cudaMalloc(&d_shift, shift.size_bytes()), "cudaMalloc");
        detail::check(cudaMalloc(&d_out, n * sizeof(double)), "cudaMalloc");
        cudaMemcpy(d_V, V.data(), V.size_bytes(), cudaMemcpyHostToDevice);
        cudaMemcpy(d_shift, shift.data(), shift.size_bytes(), cudaMemcpyHostToDevice);
        const unsigned threads = 256;
        const auto blocks = static_cast<unsigned>((n + threads - 1) / threads);
        sobol_kernel<<<blocks, threads>>>(d_V, d_shift, dim, first_point, n_points, d_out);
        const cudaError_t launch = cudaGetLastError();
        const cudaError_t copy = cudaMemcpy(host.data(), d_out, n * sizeof(double), cudaMemcpyDeviceToHost);
        cudaFree(d_V);
        cudaFree(d_shift);
        cudaFree(d_out);
        detail::check(launch, "kernel launch");
        detail::check(copy, "cudaMemcpy");
        return host;
    }

    std::vector<double> philox_uniforms(uint64_t seed, uint64_t first_path, uint32_t n_paths, uint32_t draws,
                                        int device)
    {
        detail::require_device(device);
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
