#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/utils/dual.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        using L = ScriptLimits;

        /// Device copies of host arrays, freed together.
        struct DeviceArrays
        {
            std::vector<void *> owned;
            DeviceArrays() = default;
            DeviceArrays(const DeviceArrays &) = delete;
            DeviceArrays &operator=(const DeviceArrays &) = delete;
            ~DeviceArrays()
            {
                for (void *p : owned)
                    cudaFree(p);
            }
            template <class T>
            const T *put(const std::vector<T> &h)
            {
                if (h.empty())
                    return nullptr;
                T *d = nullptr;
                detail::check(cudaMalloc(&d, h.size() * sizeof(T)), "cudaMalloc");
                owned.push_back(d);
                detail::check(cudaMemcpy(d, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice), "cudaMemcpy");
                return d;
            }
            template <class T>
            T *alloc(std::size_t n)
            {
                T *d = nullptr;
                detail::check(cudaMalloc(&d, n * sizeof(T)), "cudaMalloc");
                owned.push_back(d);
                return d;
            }
        };

        /// Check the request, select the device and copy the script and
        /// model: the view every kernel below runs.
        mc::ScriptPathView device_view(const ScriptGpuRequest &req, DeviceArrays &mem)
        {
            if (!req.program || !req.model)
                throw InvalidInput("simulate_script: program and model are required");
            const scripting::Program &p = *req.program;
            const DeviceModel &dm = *req.model;
            const std::string why = script_gpu_unsupported(p, dm, req.discount_mats);
            if (!why.empty())
                throw InvalidInput("GPU: " + why);
            if (req.discount_mats.size() != p.n_events())
                throw InvalidInput("simulate_script: one discount list per event");
            detail::require_device(req.device);
            detail::check(cudaSetDevice(req.device), "cudaSetDevice");

            std::vector<int> disc_begin{0};
            std::vector<Time> disc_mats;
            for (const auto &mats : req.discount_mats)
            {
                disc_mats.insert(disc_mats.end(), mats.begin(), mats.end());
                disc_begin.push_back(static_cast<int>(disc_mats.size()));
            }

            mc::ScriptPathView v;
            v.prog = {mem.put(p.code), mem.put(p.ifs), mem.put(p.aff)};
            v.event_begin = mem.put(p.event_begin);
            v.n_vars = p.n_vars;
            v.baseline = mem.put(req.baseline);
            v.kind = dm.kind;
            v.n_assets = dm.n_assets;
            v.r = dm.r;
            v.q = dm.q;
            v.s0 = mem.put(dm.s0);
            v.chol = mem.put(dm.chol);
            v.divs = mem.put(dm.divs);
            v.vols = mem.put(dm.vols);
            v.heston = dm.heston;
            v.grid = {mem.put(dm.K), static_cast<int>(dm.K.size()), mem.put(dm.T_grid),
                      static_cast<int>(dm.T_grid.size()), mem.put(dm.grid)};
            v.t = mem.put(dm.t);
            v.draws = mem.put(dm.draws);
            v.event = mem.put(dm.event);
            v.drift = mem.put(dm.drift);
            v.vol_sqrt_dt = mem.put(dm.vol_sqrt_dt);
            v.n_steps = static_cast<int>(dm.t.size());
            v.factors = dm.factors;
            v.stride = dm.stride;
            v.disc_begin = mem.put(disc_begin);
            v.disc_mats = mem.put(disc_mats);
            v.controls = req.controls;
            return v;
        }

        // ── prices ──────────────────────────────────────────────────────────

        /// One unit: a path, or the antithetic pair; with controls, the
        /// regression sample (payoff, controls), averaged over the pair.
        template <bool Controlled>
        struct ScriptUnit
        {
            mc::ScriptPathView view;
            mc::PathDraws draws;
            bool antithetic;
            uint64_t per_replicate; ///< units per replicate

            using Out = std::conditional_t<Controlled, MultiControlAccumulator::Sample, double>;

            /// Unit u of replicate `rep` (its Philox counter: rep * n + u).
            __device__ Out operator()(uint64_t rep, uint64_t u) const
            {
                const uint64_t first = rep * per_replicate;
                const mc::ScriptModelInputs<double> in = mc::script_inputs<double>(view);
                mc::PathDraws d = draws;
                const uint64_t stratum = draws.strata ? mc::stratum_of(u, draws.strata) : 0;
                d.begin(first + u, stratum);
                double x[MultiControlAccumulator::kMaxControls] = {};
                const double a = mc::script_path<double>(view, in, d, 1.0, Controlled ? x : nullptr);
                if constexpr (!Controlled)
                {
                    if (!antithetic)
                        return a;
                    d.begin(first + u, stratum);
                    const double b = mc::script_path<double>(view, in, d, -1.0, nullptr);
                    return 0.5 * (a + b);
                }
                else
                {
                    Out s;
                    s.v[0] = a;
                    for (int c = 0; c < view.controls.n; ++c)
                        s.v[1 + c] = x[c];
                    if (!antithetic)
                        return s;
                    double y[MultiControlAccumulator::kMaxControls] = {};
                    d.begin(first + u, stratum);
                    const double b = mc::script_path<double>(view, in, d, -1.0, y);
                    s.v[0] = 0.5 * (s.v[0] + b);
                    for (int c = 0; c < view.controls.n; ++c)
                        s.v[1 + c] = 0.5 * (s.v[1 + c] + y[c]);
                    return s;
                }
            }
        };

        // ── forward-mode risks ──────────────────────────────────────────────

        template <int N>
        struct DualStats
        {
            WelfordAccumulator w[N + 1];

            __host__ __device__ void add(const Dual<N> &x)
            {
                w[0].add(x.v);
                for (int i = 0; i < N; ++i)
                    w[i + 1].add(x.d[i]);
            }
            __host__ __device__ void merge(const DualStats &o)
            {
                for (int i = 0; i <= N; ++i)
                    w[i].merge(o.w[i]);
            }
        };

        template <int N>
        struct DualUnit
        {
            mc::ScriptPathView view;
            mc::PathDraws draws;

            __device__ Dual<N> operator()(uint64_t u) const
            {
                const mc::ScriptModelInputs<Dual<N>> in = mc::script_dual_inputs<N>(view);
                mc::PathDraws d = draws;
                d.begin(u);
                return mc::script_path<Dual<N>>(view, in, d, 1.0, nullptr);
            }
        };

        template <int N>
        std::vector<WelfordAccumulator> run_duals(const ScriptGpuRequest &req, const mc::ScriptPathView &v, int dirs)
        {
            DualUnit<N> unit{v, {}};
            unit.draws.seed = req.seed;
            unit.draws.stride = v.stride;
            const DualStats<N> st =
                detail::run_logical_blocks<DualStats<N>>(req.device, req.n_units, unit, req.max_blocks_per_launch);
            return std::vector<WelfordAccumulator>(st.w, st.w + 1 + dirs);
        }

        // ── per-path adjoint, local vol ─────────────────────────────────────

        /// Thread `row` of a launch keeps its trail and its gradient row in
        /// global memory, element k at base[k * rows + row] (coalesced across
        /// a warp). Every row is private to its thread: no atomics, so the
        /// sums do not depend on scheduling -- bit-reproducible.
        struct AdjointUnit
        {
            mc::ScriptPathView view;
            mc::PathDraws draws;
            double *trail;
            double *grad;
            long rows;

            __device__ double operator()(uint64_t u) const
            {
                const long row = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
                mc::AdjointScratch w{{trail + row, rows, 0}, {grad + row, rows}};
                return mc::script_lv_adjoint_path(view, draws, u, 1.0, w);
            }
        };

        /// out[p * n_warps + w] = sum over the 32 lanes of warp w of
        /// grad[p * rows + 32 w + lane], in lane order.
        __global__ void fold_warps(const double *grad, long rows, int n_params, long n_warps, double *out)
        {
            const long idx = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (idx >= n_warps * n_params)
                return;
            const long w = idx % n_warps;
            const long p = idx / n_warps;
            const double *g = grad + p * rows + w * 32;
            double s = 0.0;
            for (int lane = 0; lane < 32; ++lane)
                s += g[lane];
            out[p * n_warps + w] = s;
        }

        /// Units the warp `w` of logical block `block` handled.
        long long warp_units(uint64_t block, int w, uint64_t n_units)
        {
            using LB = mc::LogicalBlocks;
            long long n = 0;
            const uint64_t base = block * LB::kUnitsPerBlock;
            for (int lane = 0; lane < LB::kWarpSize; ++lane)
                for (int k = 0; k < LB::kUnitsPerThread; ++k)
                    if (base + static_cast<uint64_t>(w * LB::kWarpSize + lane) +
                            static_cast<uint64_t>(k) * LB::kThreadsPerBlock <
                        n_units)
                        ++n;
            return n;
        }
    } // namespace

    ScriptGpuStats simulate_script(const ScriptGpuRequest &req)
    {
        DeviceArrays mem;
        const mc::ScriptPathView v = device_view(req, mem);
        const detail::StackLimitRestore release_stack;

        mc::PathDraws draws;
        draws.seed = req.seed;
        draws.stride = v.stride;
        int replicates = 1;
        if (req.stratified)
        {
            std::vector<Time> times;
            for (std::size_t s = 0; s < req.model->t.size(); ++s)
                if (req.model->draws[s])
                    times.push_back(req.model->t[s]);
            draws.strata = req.n_units;
            draws.times = mem.put(times);
            draws.n_steps = static_cast<int>(times.size());
            replicates = std::max(1, req.replicates);
        }

        // All replicates in one grid: each is reduced by its own tree, and
        // together they fill the device (one replicate of a 10^5-path run
        // is a single logical block).
        ScriptGpuStats out;
        const auto reps = static_cast<uint64_t>(replicates);
        if (v.controls.n > 0 && req.stratified)
        {
            const ScriptUnit<true> unit{v, draws, req.antithetic, req.n_units};
            out.stratified_controlled = detail::run_logical_block_segments<StratifiedControlAccumulator>(
                req.device, reps, req.n_units, unit, req.max_blocks_per_launch);
        }
        else if (v.controls.n > 0)
        {
            const ScriptUnit<true> unit{v, draws, req.antithetic, req.n_units};
            out.controlled = detail::run_logical_block_segments<MultiControlAccumulator>(
                req.device, reps, req.n_units, unit, req.max_blocks_per_launch);
        }
        else
        {
            const ScriptUnit<false> unit{v, draws, req.antithetic, req.n_units};
            out.plain = detail::run_logical_block_segments<WelfordAccumulator>(req.device, reps, req.n_units, unit,
                                                                               req.max_blocks_per_launch);
        }
        return out;
    }

    std::vector<WelfordAccumulator> simulate_script_duals(const ScriptGpuRequest &req)
    {
        DeviceArrays mem;
        const mc::ScriptPathView v = device_view(req, mem);
        const detail::StackLimitRestore release_stack;
        const int dirs = mc::dual_directions(v.kind, v.n_assets);
        if (dirs <= 0 || dirs > 8)
            throw InvalidInput("GPU: forward-mode risks cover Black-Scholes (one or two assets) and Heston");
        return dirs <= 4 ? run_duals<4>(req, v, dirs) : run_duals<8>(req, v, dirs);
    }

    ScriptAdjointGpuResult simulate_script_adjoint(const ScriptGpuRequest &req)
    {
        DeviceArrays mem;
        const mc::ScriptPathView v = device_view(req, mem);
        const detail::StackLimitRestore release_stack;
        if (v.kind != DeviceModel::Kind::LocalVol)
            throw InvalidInput("GPU: the per-path adjoint covers local vol");
        using LB = mc::LogicalBlocks;

        const int n_params = 3 + v.grid.nK * v.grid.nT;
        const long trail_len = mc::lv_trail_bound(*req.program, v.n_steps);

        // Launch size from the memory a thread needs (its trail and its
        // gradient row), never a default: at most half of what is free (the
        // cards are shared), and the warp partials on top.
        const uint64_t n_blocks = LB::count(req.n_units);
        const std::size_t per_block =
            static_cast<std::size_t>(LB::kThreadsPerBlock) * static_cast<std::size_t>(trail_len + n_params) *
                sizeof(double) +
            static_cast<std::size_t>(LB::kWarpsPerBlock) * static_cast<std::size_t>(n_params) * sizeof(double);
        uint64_t per_launch = std::max<uint64_t>(1, free_memory(req.device) / 2 / per_block);
        if (req.max_blocks_per_launch > 0)
            per_launch = std::min(per_launch, req.max_blocks_per_launch);
        per_launch = std::min<uint64_t>(per_launch, std::max<uint64_t>(n_blocks, 1));
        const long rows = static_cast<long>(per_launch) * LB::kThreadsPerBlock;
        const long max_warps = static_cast<long>(per_launch) * LB::kWarpsPerBlock;

        double *d_trail = mem.alloc<double>(static_cast<std::size_t>(rows) * static_cast<std::size_t>(trail_len));
        double *d_grad = mem.alloc<double>(static_cast<std::size_t>(rows) * static_cast<std::size_t>(n_params));
        double *d_warps = mem.alloc<double>(static_cast<std::size_t>(max_warps) * static_cast<std::size_t>(n_params));
        detail::check(cudaMemset(d_grad, 0, static_cast<std::size_t>(rows) * n_params * sizeof(double)), "cudaMemset");

        AdjointUnit unit{v, {}, d_trail, d_grad, rows};
        unit.draws.seed = req.seed;
        unit.draws.stride = v.stride;

        // Batches of one warp (512 paths): the risks' mean is the total over
        // the paths, their error the spread of the batch means (ratio
        // estimator: batches at the tail may be shorter). Sums shifted by
        // the first batch's mean against cancellation.
        std::vector<double> host(static_cast<std::size_t>(max_warps) * static_cast<std::size_t>(n_params));
        std::vector<double> shift(static_cast<std::size_t>(n_params), 0.0), s1(shift), s2(shift), s1n(shift);
        double total_units = 0.0, sum_n2 = 0.0;
        long long batches = 0;
        auto after = [&](uint64_t first, uint64_t count)
        {
            const long n_warps = static_cast<long>(count) * LB::kWarpsPerBlock;
            const long total = n_warps * n_params;
            fold_warps<<<static_cast<unsigned>((total + 255) / 256), 256>>>(d_grad, rows, n_params, n_warps, d_warps);
            detail::check(cudaGetLastError(), "fold_warps launch");
            detail::check(cudaMemcpy(host.data(), d_warps, static_cast<std::size_t>(total) * sizeof(double),
                                     cudaMemcpyDeviceToHost),
                          "cudaMemcpy");
            detail::check(cudaMemset(d_grad, 0, static_cast<std::size_t>(rows) * n_params * sizeof(double)),
                          "cudaMemset");
            for (long w = 0; w < n_warps; ++w)
            {
                const long long nb = warp_units(first + static_cast<uint64_t>(w / LB::kWarpsPerBlock),
                                                static_cast<int>(w % LB::kWarpsPerBlock), req.n_units);
                if (nb == 0)
                    continue;
                const double n = static_cast<double>(nb);
                if (batches == 0)
                    for (int p = 0; p < n_params; ++p)
                        shift[static_cast<std::size_t>(p)] = host[static_cast<std::size_t>(p) * n_warps + w] / n;
                for (int p = 0; p < n_params; ++p)
                {
                    const auto pi = static_cast<std::size_t>(p);
                    const double y = host[pi * n_warps + w] / n - shift[pi]; // batch mean, shifted
                    s1[pi] += n * y;
                    s1n[pi] += n * n * y;
                    s2[pi] += n * n * y * y;
                }
                total_units += n;
                sum_n2 += n * n;
                ++batches;
            }
        };
        ScriptAdjointGpuResult res;
        res.price = detail::run_logical_blocks<WelfordAccumulator>(req.device, req.n_units, unit, per_launch, after);

        res.batches = batches;
        res.risks.resize(static_cast<std::size_t>(n_params));
        res.risk_std_errors.assign(static_cast<std::size_t>(n_params), 0.0);
        for (std::size_t p = 0; p < res.risks.size(); ++p)
        {
            const double mu = total_units > 0.0 ? s1[p] / total_units : 0.0;
            res.risks[p] = shift[p] + mu;
            if (batches > 1)
            {
                const double b = static_cast<double>(batches);
                const double ss = s2[p] - 2.0 * mu * s1n[p] + mu * mu * sum_n2; // sum n^2 (ybar - mu)^2
                res.risk_std_errors[p] =
                    std::sqrt(std::max(0.0, ss) * b / (b - 1.0)) / total_units;
            }
        }
        return res;
    }

} // namespace quantModeling::gpu
