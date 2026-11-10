// Uniform generation into device memory: our Sobol and Philox against
// NVIDIA's cuRAND host API, on one device.
//
//   build-cuda/qm_gpu_rng_bench [points=1048576] [dims=128]
//
// Why it exists: deciding whether to replace our generators with cuRAND
// (.claude/skills/hpc-finance/libraries.md). Measured on a V100 on
// 2026-11-10, 2^20 points x 128 dims (1.07 GB of doubles):
//
//   ours   Sobol, random access (sobol_point_bits)   249 GB/s
//   ours   Sobol, branchless, directions in smem     456-500 GB/s (same bits)
//   ours   Philox4x32-10                             426-470 GB/s
//   cuRAND Sobol64 / scrambled Sobol64 / Sobol32     ~820 GB/s (Gray-code stepping)
//   cuRAND Philox4x32-10                             300 GB/s
//
// (ranges: two runs, the build of the target and a standalone build.)
//
// Both Philox versions put 64 random bits in each double. The kernels here
// write every number to memory; the engine's path kernels draw them inline,
// where generation is only part of the cost.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <cuda_runtime.h>
#include <curand.h>

#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/sobol.hpp"

using namespace quantModeling;

namespace
{
    /// Dimension-major, like cuRAND: number i is point i % n of dimension i / n.
    __global__ void sobol_random_access(const uint32_t *V, const uint32_t *shift, int dim, uint32_t n, double *out)
    {
        const uint64_t i = blockIdx.x * static_cast<uint64_t>(blockDim.x) + threadIdx.x;
        if (i >= static_cast<uint64_t>(n) * dim)
            return;
        const auto d = static_cast<uint32_t>(i / n), p = static_cast<uint32_t>(i % n);
        out[i] = sobol_to_uniform(sobol_point_bits(V + 32 * d, p), shift[d]);
    }

    /// The same bits without the data-dependent loop: one grid row per
    /// dimension, its 32 directions in shared memory, all 32 bits unrolled.
    __global__ void sobol_branchless(const uint32_t *V, const uint32_t *shift, uint32_t n, double *out)
    {
        __shared__ uint32_t v[32];
        const uint32_t d = blockIdx.y;
        if (threadIdx.x < 32)
            v[threadIdx.x] = V[32 * d + threadIdx.x];
        __syncthreads();
        const uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
        if (p >= n)
            return;
        const uint32_t g = p ^ (p >> 1);
        uint32_t x = 0;
#pragma unroll
        for (int b = 0; b < 32; ++b)
            x ^= v[b] & (0u - ((g >> b) & 1u));
        out[static_cast<size_t>(d) * n + p] = sobol_to_uniform(x, shift[d]);
    }

    __global__ void philox(uint64_t seed, uint32_t n, int draws, double *out)
    {
        const uint64_t i = blockIdx.x * static_cast<uint64_t>(blockDim.x) + threadIdx.x;
        if (i >= static_cast<uint64_t>(n) * draws)
            return;
        out[i] = philox_uniform(seed, i / draws, static_cast<uint32_t>(i % draws));
    }

    /// Best of 5 timed runs after one warm-up.
    template <class F>
    float best_ms(F f)
    {
        cudaEvent_t a, b;
        cudaEventCreate(&a);
        cudaEventCreate(&b);
        f();
        cudaDeviceSynchronize();
        float best = 1e30f;
        for (int r = 0; r < 5; ++r)
        {
            cudaEventRecord(a);
            f();
            cudaEventRecord(b);
            cudaEventSynchronize(b);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, a, b);
            best = ms < best ? ms : best;
        }
        cudaEventDestroy(a);
        cudaEventDestroy(b);
        return best;
    }
} // namespace

int main(int argc, char **argv)
{
    const auto n = static_cast<uint32_t>(argc > 1 ? std::atol(argv[1]) : 1L << 20);
    const int dim = argc > 2 ? std::atoi(argv[2]) : 128;
    const size_t N = static_cast<size_t>(n) * dim;
    const double gb = static_cast<double>(N) * sizeof(double) / 1e9;

    double *d_out = nullptr;
    uint32_t *d_V = nullptr, *d_shift = nullptr;
    const SobolSequence seq(dim, 7);
    const auto V = seq.directions();
    const auto shift = seq.shifts();
    if (cudaMalloc(&d_out, N * sizeof(double)) != cudaSuccess || cudaMalloc(&d_V, V.size_bytes()) != cudaSuccess ||
        cudaMalloc(&d_shift, shift.size_bytes()) != cudaSuccess)
    {
        std::printf("not enough device memory for %zu doubles\n", N);
        return 1;
    }
    cudaMemcpy(d_V, V.data(), V.size_bytes(), cudaMemcpyHostToDevice);
    cudaMemcpy(d_shift, shift.data(), shift.size_bytes(), cudaMemcpyHostToDevice);

    const unsigned threads = 256;
    const auto blocks = static_cast<unsigned>((N + threads - 1) / threads);
    const dim3 rows((n + threads - 1) / threads, dim);
    const auto report = [&](const char *name, float ms)
    { std::printf("  %-36s %8.2f ms  %7.1f Gnum/s  %5.0f GB/s\n", name, ms, N / ms / 1e6, gb / ms * 1e3); };

    std::printf("%u points x %d dims = %zu doubles (%.2f GB)\n", n, dim, N, gb);
    report("ours   Sobol, random access",
           best_ms([&] { sobol_random_access<<<blocks, threads>>>(d_V, d_shift, dim, n, d_out); }));
    std::vector<double> a(N), b(N);
    cudaMemcpy(a.data(), d_out, N * sizeof(double), cudaMemcpyDeviceToHost);
    report("ours   Sobol, branchless (smem)", best_ms([&] { sobol_branchless<<<rows, threads>>>(d_V, d_shift, n, d_out); }));
    cudaMemcpy(b.data(), d_out, N * sizeof(double), cudaMemcpyDeviceToHost);
    std::printf("    branchless bits == random-access bits: %s\n", a == b ? "yes" : "NO");
    report("ours   Philox4x32-10", best_ms([&] { philox<<<blocks, threads>>>(42, n, dim, d_out); }));

    struct Generator
    {
        curandRngType_t type;
        const char *name;
        bool quasi;
    };
    const Generator generators[] = {{CURAND_RNG_QUASI_SOBOL64, "cuRAND Sobol64", true},
                                    {CURAND_RNG_QUASI_SCRAMBLED_SOBOL64, "cuRAND scrambled Sobol64", true},
                                    {CURAND_RNG_QUASI_SOBOL32, "cuRAND Sobol32", true},
                                    {CURAND_RNG_PSEUDO_PHILOX4_32_10, "cuRAND Philox4x32-10", false}};
    for (const Generator &g : generators)
    {
        curandGenerator_t gen;
        curandCreateGenerator(&gen, g.type);
        if (g.quasi)
            curandSetQuasiRandomGeneratorDimensions(gen, static_cast<unsigned>(dim));
        else
            curandSetPseudoRandomGeneratorSeed(gen, 42);
        report(g.name, best_ms([&] { curandGenerateUniformDouble(gen, d_out, N); }));
        curandDestroyGenerator(gen);
    }
    cudaFree(d_out);
    cudaFree(d_V);
    cudaFree(d_shift);
    return 0;
}
