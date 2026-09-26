#include "quantModeling/gpu/paths.hpp"
#include "quantModeling/utils/philox.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        struct DeviceArgs
        {
            PathModel kind;
            Real s0, r, q;
            mc::HestonParamsT<Real> heston;
            mc::GridView<Real> grid;
            const Time *times;
            int n_steps;
            int factors;
            uint64_t seed;
        };

        __global__ void terminal_kernel(DeviceArgs a, uint32_t n_paths, double *out)
        {
            const uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
            if (p >= n_paths)
                return;
            Real S = a.s0;
            Real v = a.heston.v0;
            Time t_prev = 0.0;
            for (int s = 0; s < a.n_steps; ++s)
            {
                const Time t = a.times[s];
                const Time dt = t - t_prev;
                const uint32_t j = static_cast<uint32_t>(s * a.factors);
                const double z0 = inverse_normal_cdf(philox_uniform(a.seed, p, j));
                if (a.kind == PathModel::LocalVol)
                {
                    mc::local_vol_step(a.grid, a.r, a.q, S, t, dt, z0);
                }
                else
                {
                    const double z1 = inverse_normal_cdf(philox_uniform(a.seed, p, j + 1));
                    if (a.kind == PathModel::SLV)
                    {
                        mc::slv_step(a.grid, a.heston, a.r, a.q, S, v, t, dt, z0, z1);
                    }
                    else
                    {
                        const double sqdt = sqrt(dt);
                        const Real v_plus = fmax(v, mc::variance_floor());
                        const Real sqrt_v_plus = sqrt(v_plus);
                        const Real zero = 0.0;
                        S = S * exp(mc::heston_log_return(a.r, a.q, zero, zero, v_plus, sqrt_v_plus, sqdt, dt, z0));
                        mc::heston_variance_step(a.heston, v, v_plus, sqrt_v_plus, sqdt, dt, z0, z1);
                    }
                }
                t_prev = t;
            }
            out[p] = S;
        }

        template <class T>
        T *to_device(const std::vector<T> &h)
        {
            T *d = nullptr;
            if (h.empty())
                return d;
            detail::check(cudaMalloc(&d, h.size() * sizeof(T)), "cudaMalloc");
            detail::check(cudaMemcpy(d, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice), "cudaMemcpy");
            return d;
        }
    } // namespace

    std::vector<double> terminal_spots(const PathModelSpec &model, const std::vector<Time> &times, uint32_t n_paths,
                                       uint64_t seed, int device)
    {
        detail::require_device(device);
        detail::check(cudaSetDevice(device), "cudaSetDevice");
        if (model.kind != PathModel::Heston &&
            (model.K.size() < 2 || model.T_grid.size() < 2 || model.grid.size() != model.K.size() * model.T_grid.size()))
            throw InvalidInput("terminal_spots: the grid needs >= 2 strikes and times, and K x T values");

        std::vector<double> host(n_paths);
        if (n_paths == 0 || times.empty())
            return host;
        Real *dK = to_device(model.K), *dT = to_device(model.T_grid), *dG = to_device(model.grid);
        Time *dTimes = to_device(times);
        double *dOut = nullptr;
        detail::check(cudaMalloc(&dOut, n_paths * sizeof(double)), "cudaMalloc");

        DeviceArgs a{model.kind,
                     model.s0,
                     model.r,
                     model.q,
                     model.heston,
                     {dK, static_cast<int>(model.K.size()), dT, static_cast<int>(model.T_grid.size()), dG},
                     dTimes,
                     static_cast<int>(times.size()),
                     model.factors(),
                     seed};
        const unsigned threads = 256;
        terminal_kernel<<<(n_paths + threads - 1) / threads, threads>>>(a, n_paths, dOut);
        const cudaError_t launch = cudaGetLastError();
        const cudaError_t copy = cudaMemcpy(host.data(), dOut, n_paths * sizeof(double), cudaMemcpyDeviceToHost);
        for (void *ptr : {static_cast<void *>(dK), static_cast<void *>(dT), static_cast<void *>(dG),
                          static_cast<void *>(dTimes), static_cast<void *>(dOut)})
            cudaFree(ptr);
        detail::check(launch, "kernel launch");
        detail::check(copy, "cudaMemcpy");
        return host;
    }

} // namespace quantModeling::gpu
