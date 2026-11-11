// Hand-written reference paths on one V100 (blueprint/wp/26-performance.md,
// lot P0): the ceiling for the script engine's GPU rows of
// benchmarks/perf_matrix.py. One thread per unit (an antithetic pair),
// grid-stride; per-thread sums folded on the host in thread order.
//
//   build-cuda/qm_gpu_reference_bench [target_rel_se=1e-3] [device=0]

#include "reference_cases.hpp"

#include <cstdlib>

#include <cuda_runtime.h>

using namespace qm_reference;

namespace
{
    __global__ void units_kernel(Product product, Terms terms, LocalVolGrid grid, bool local_vol, uint64_t units,
                                 double *sums, double *squares)
    {
        const uint64_t t = blockIdx.x * static_cast<uint64_t>(blockDim.x) + threadIdx.x;
        const uint64_t stride = static_cast<uint64_t>(gridDim.x) * blockDim.x;
        double s = 0.0, q = 0.0;
        for (uint64_t u = t; u < units; u += stride)
        {
            const double x = unit(product, terms, local_vol ? &grid : nullptr, 1, u);
            s += x;
            q += x * x;
        }
        sums[t] = s;
        squares[t] = q;
    }

    template <class T>
    T *to_device(const std::vector<T> &h)
    {
        T *d = nullptr;
        cudaMalloc(&d, h.size() * sizeof(T));
        cudaMemcpy(d, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice);
        return d;
    }
} // namespace

int main(int argc, char **argv)
{
    const double target = argc > 1 ? std::atof(argv[1]) : 1e-3;
    const int device = argc > 2 ? std::atoi(argv[2]) : 0;
    cudaSetDevice(device);
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, device);

    const Grid g = local_vol_grid();
    double *dK = to_device(g.K), *dT = to_device(g.T), *dS = to_device(g.sigma);
    const LocalVolGrid view{dK, static_cast<int>(g.K.size()), dT, static_cast<int>(g.T.size()), dS};

    const unsigned threads = 256;
    const unsigned blocks = static_cast<unsigned>(prop.multiProcessorCount) * 8;
    const std::size_t lanes = static_cast<std::size_t>(threads) * blocks;
    double *d_sums = nullptr, *d_squares = nullptr;
    cudaMalloc(&d_sums, lanes * sizeof(double));
    cudaMalloc(&d_squares, lanes * sizeof(double));
    std::vector<double> sums(lanes), squares(lanes);

    std::printf("Reference paths on %s, time to a relative std error of %.0e\n", prop.name, target);
    for (const Case &c : cases())
    {
        time_to_error(c.name, target,
                      [&](uint64_t units)
                      {
                          Estimate e;
                          e.seconds = seconds(
                              [&]
                              {
                                  units_kernel<<<blocks, threads>>>(c.product, c.terms, view, c.local_vol, units,
                                                                    d_sums, d_squares);
                                  cudaMemcpy(sums.data(), d_sums, lanes * sizeof(double), cudaMemcpyDeviceToHost);
                                  cudaMemcpy(squares.data(), d_squares, lanes * sizeof(double),
                                             cudaMemcpyDeviceToHost);
                              });
                          double s = 0.0, q = 0.0;
                          for (std::size_t k = 0; k < lanes; ++k)
                          {
                              s += sums[k];
                              q += squares[k];
                          }
                          const double n = static_cast<double>(units);
                          e.mean = s / n;
                          e.se = std::sqrt(std::max(0.0, q / n - e.mean * e.mean) / (n - 1));
                          return e;
                      });
    }
    for (void *p : {static_cast<void *>(dK), static_cast<void *>(dT), static_cast<void *>(dS),
                    static_cast<void *>(d_sums), static_cast<void *>(d_squares)})
        cudaFree(p);
    return 0;
}
