#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/sobol_bridge.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/utils/dual.hpp"

#include "logical_blocks.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        using L = ScriptLimits;
        using LB = mc::LogicalBlocks;

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
        mc::ScriptPathView device_view(const ScriptGpuRequest &req, DeviceArrays &mem, int device)
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
            detail::require_device(device);
            detail::check(cudaSetDevice(device), "cudaSetDevice");

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

        /**
         * How a unit gets its draws: Philox (plain, stratified, shifted for
         * importance sampling), or a Sobol point through the Brownian bridge.
         * A Sobol point and its gaussians live in the unit's thread's scratch
         * rows in global memory (element j at base[j * rows + row]).
         */
        struct UnitDraws
        {
            mc::PathDraws base;
            mc::SobolBridgeView sobol; ///< dim == 0: Philox
            double *scratch = nullptr;
            long rows = 0;

            /// The draws of unit u of replicate `rep` (Philox counter first + u).
            __device__ mc::PathDraws begin(uint64_t rep, uint64_t u, uint64_t first) const
            {
                mc::PathDraws d = base;
                d.begin(first + u, base.strata ? mc::stratum_of(u, base.strata) : 0);
                if (sobol.dim > 0)
                {
                    const long row = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
                    double *pt = scratch + row;
                    double *out = scratch + static_cast<long>(sobol.dim) * rows + row;
                    mc::sobol_bridged_gaussians(sobol, rep, static_cast<uint32_t>(u), pt, rows, out, rows);
                    d.given = out;
                    d.given_stride = rows;
                }
                return d;
            }
        };

        /// What a request's draws need on the device; `blocks_per_launch`
        /// comes back capped by the Sobol scratch the memory allows.
        struct DrawSetup
        {
            UnitDraws draws;
            uint64_t replicates = 1;
            uint64_t blocks_per_launch = 0;
        };

        DrawSetup draw_setup(const ScriptGpuRequest &req, const mc::ScriptPathView &v, DeviceArrays &mem, int device,
                             std::size_t other_bytes_per_block = 0)
        {
            DrawSetup s;
            s.draws.base.seed = req.seed;
            s.draws.base.stride = v.stride;
            s.blocks_per_launch = req.max_blocks_per_launch;
            if (req.stratified || !req.is_theta.empty())
            {
                std::vector<Time> times;
                for (std::size_t k = 0; k < req.model->t.size(); ++k)
                    if (req.model->draws[k])
                        times.push_back(req.model->t[k]);
                s.draws.base.times = mem.put(times);
                s.draws.base.n_steps = static_cast<int>(times.size());
            }
            if (req.stratified)
            {
                s.draws.base.strata = req.n_units;
                s.replicates = static_cast<uint64_t>(std::max(1, req.replicates));
            }
            if (!req.is_theta.empty())
            {
                s.draws.base.theta = mem.put(req.is_theta);
                s.draws.base.n_theta = static_cast<int>(req.is_theta.size());
            }

            std::size_t per_block = other_bytes_per_block;
            if (req.sobol)
            {
                const mc::SobolTables &t = *req.sobol;
                mc::SobolBridgeView &sv = s.draws.sobol;
                sv = t.view(); // host pointers, replaced below
                sv.V = mem.put(t.V);
                sv.shift = mem.put(t.shift);
                sv.bridge_index = mem.put(t.bridge_index);
                sv.left_index = mem.put(t.left_index);
                sv.right_index = mem.put(t.right_index);
                sv.left_weight = mem.put(t.left_weight);
                sv.right_weight = mem.put(t.right_weight);
                sv.std_dev = mem.put(t.std_dev);
                sv.inv_sqrt_dt = mem.put(t.inv_sqrt_dt);
                s.replicates = static_cast<uint64_t>(std::max(1, t.replicates));
                per_block += static_cast<std::size_t>(LB::kThreadsPerBlock) * 2u * static_cast<std::size_t>(t.dim) *
                             sizeof(double);
            }
            if (per_block > 0)
            {
                // Launch size from the memory a thread needs, never a default:
                // at most half of what is free (the cards are shared).
                const uint64_t n_blocks = LB::count(req.n_units) * s.replicates;
                uint64_t per_launch = std::max<uint64_t>(1, free_memory(device) / 2 / per_block);
                if (s.blocks_per_launch > 0)
                    per_launch = std::min(per_launch, s.blocks_per_launch);
                s.blocks_per_launch = std::min<uint64_t>(per_launch, std::max<uint64_t>(n_blocks, 1));
            }
            if (req.sobol)
            {
                s.draws.rows = static_cast<long>(s.blocks_per_launch) * LB::kThreadsPerBlock;
                s.draws.scratch = mem.alloc<double>(static_cast<std::size_t>(s.draws.rows) * 2u *
                                                    static_cast<std::size_t>(req.sobol->dim));
            }
            return s;
        }

        // ── prices ──────────────────────────────────────────────────────────

        /// One unit: a path, or the antithetic pair; with controls, the
        /// regression sample (payoff, controls), averaged over the pair.
        /// Importance sampling weights each path by its likelihood ratio.
        template <bool Controlled>
        struct ScriptUnit
        {
            mc::ScriptPathView view;
            UnitDraws draws;
            bool antithetic;
            uint64_t per_replicate; ///< units per replicate

            using Out = std::conditional_t<Controlled, MultiControlAccumulator::Sample, double>;

            /// Unit u of replicate `rep` (its Philox counter: rep * n + u).
            __device__ Out operator()(uint64_t rep, uint64_t u) const
            {
                const mc::ScriptModelInputs<double> in = mc::script_inputs<double>(view);
                const mc::PathDraws d0 = draws.begin(rep, u, rep * per_replicate);
                double x[MultiControlAccumulator::kMaxControls] = {};
                mc::PathDraws d = d0;
                const double a = mc::script_path<double>(view, in, d, 1.0, Controlled ? x : nullptr);
                const double wa = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                if constexpr (!Controlled)
                {
                    if (!antithetic)
                        return a * wa;
                    d = d0;
                    const double b = mc::script_path<double>(view, in, d, -1.0, nullptr);
                    const double wb = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                    return 0.5 * (a * wa + b * wb);
                }
                else
                {
                    Out s;
                    s.v[0] = a * wa;
                    for (int c = 0; c < view.controls.n; ++c)
                        s.v[1 + c] = x[c] * wa;
                    if (!antithetic)
                        return s;
                    double y[MultiControlAccumulator::kMaxControls] = {};
                    d = d0;
                    const double b = mc::script_path<double>(view, in, d, -1.0, y);
                    const double wb = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                    s.v[0] = 0.5 * (s.v[0] + b * wb);
                    for (int c = 0; c < view.controls.n; ++c)
                        s.v[1 + c] = 0.5 * (s.v[1 + c] + y[c] * wb);
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
            UnitDraws draws;
            uint64_t per_replicate;

            __device__ Dual<N> operator()(uint64_t rep, uint64_t u) const
            {
                const mc::ScriptModelInputs<Dual<N>> in = mc::script_dual_inputs<N>(view);
                mc::PathDraws d = draws.begin(rep, u, rep * per_replicate);
                return mc::script_path<Dual<N>>(view, in, d, 1.0, nullptr);
            }
        };

        // ── per-path adjoint, local vol ─────────────────────────────────────

        /// Thread `row` of a launch keeps its trail and its gradient row in
        /// global memory, element k at base[k * rows + row] (coalesced across
        /// a warp). Every row is private to its thread: no atomics, so the
        /// sums do not depend on scheduling -- bit-reproducible.
        struct AdjointUnit
        {
            mc::ScriptPathView view;
            UnitDraws draws;
            double *trail;
            double *grad;
            long rows;
            uint64_t per_replicate;

            __device__ double operator()(uint64_t rep, uint64_t u) const
            {
                const long row = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
                const uint64_t first = rep * per_replicate;
                const mc::PathDraws d = draws.begin(rep, u, first);
                mc::AdjointScratch w{{trail + row, rows, 0}, {grad + row, rows}};
                return mc::script_lv_adjoint_path(view, d, first + u, 1.0, w);
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

        /// The devices a request runs on (lot G4): req.devices, or req.device.
        std::vector<int> devices_of(const ScriptGpuRequest &req)
        {
            return req.devices.empty() ? std::vector<int>{req.device} : req.devices;
        }

        /// Independent replicates of n_units units the request runs.
        uint64_t replicates_of(const ScriptGpuRequest &req)
        {
            if (req.sobol)
                return static_cast<uint64_t>(std::max(1, req.sobol->replicates));
            if (req.stratified)
                return static_cast<uint64_t>(std::max(1, req.replicates));
            return 1;
        }

        /// The request's logical blocks on its devices, each with its own
        /// copy of the script and model; per replicate, the blocks folded in
        /// block order -- whatever the number of devices.
        template <class Stats, class MakeUnit>
        std::vector<Stats> run_script_blocks(const ScriptGpuRequest &req, const MakeUnit &make_unit)
        {
            const uint64_t reps = replicates_of(req);
            const uint64_t bps = LB::count(req.n_units);
            auto job = [&](int device, uint64_t first, uint64_t last)
            {
                DeviceArrays mem;
                const mc::ScriptPathView v = device_view(req, mem, device);
                const detail::StackLimitRestore release_stack;
                const DrawSetup s = draw_setup(req, v, mem, device);
                const auto unit = make_unit(v, s);
                return detail::run_block_range<Stats>(device, first, last, bps, req.n_units, unit,
                                                      s.blocks_per_launch);
            };
            return detail::merge_segments(detail::run_on_devices<Stats>(devices_of(req), bps * reps, job), reps, bps);
        }

        template <int N>
        std::vector<std::vector<WelfordAccumulator>> run_duals(const ScriptGpuRequest &req, int dirs)
        {
            const auto st = run_script_blocks<DualStats<N>>(
                req, [&](const mc::ScriptPathView &v, const DrawSetup &s)
                { return DualUnit<N>{v, s.draws, req.n_units}; });
            std::vector<std::vector<WelfordAccumulator>> out;
            for (const DualStats<N> &r : st)
                out.emplace_back(r.w, r.w + 1 + dirs);
            return out;
        }

        /// One logical block of the adjoint: its price statistics, and the
        /// gradient sums of its 8 warps (8 x n_params, warp-major).
        struct AdjointBlock
        {
            WelfordAccumulator price;
            std::vector<double> warp_sums;
        };
    } // namespace

    ScriptGpuStats simulate_script(const ScriptGpuRequest &req)
    {
        ScriptGpuStats out;
        if (req.controls.n > 0 && req.stratified)
            out.stratified_controlled = run_script_blocks<StratifiedControlAccumulator>(
                req, [&](const mc::ScriptPathView &v, const DrawSetup &s)
                { return ScriptUnit<true>{v, s.draws, req.antithetic, req.n_units}; });
        else if (req.controls.n > 0)
            out.controlled = run_script_blocks<MultiControlAccumulator>(
                req, [&](const mc::ScriptPathView &v, const DrawSetup &s)
                { return ScriptUnit<true>{v, s.draws, req.antithetic, req.n_units}; });
        else
            out.plain = run_script_blocks<WelfordAccumulator>(
                req, [&](const mc::ScriptPathView &v, const DrawSetup &s)
                { return ScriptUnit<false>{v, s.draws, req.antithetic, req.n_units}; });
        return out;
    }

    std::vector<std::vector<WelfordAccumulator>> simulate_script_duals(const ScriptGpuRequest &req)
    {
        if (!req.model)
            throw InvalidInput("simulate_script: program and model are required");
        const int dirs = mc::dual_directions(req.model->kind, req.model->n_assets);
        if (dirs <= 0 || dirs > 8)
            throw InvalidInput("GPU: forward-mode risks cover Black-Scholes (one or two assets) and Heston");
        return dirs <= 4 ? run_duals<4>(req, dirs) : run_duals<8>(req, dirs);
    }

    ScriptAdjointGpuResult simulate_script_adjoint(const ScriptGpuRequest &req)
    {
        if (!req.model || !req.program)
            throw InvalidInput("simulate_script: program and model are required");
        if (req.model->kind != DeviceModel::Kind::LocalVol)
            throw InvalidInput("GPU: the per-path adjoint covers local vol");
        if (req.stratified || !req.is_theta.empty())
            throw InvalidInput("GPU: the per-path adjoint runs pseudo-random or Sobol paths");

        const int n_params = 3 + static_cast<int>(req.model->K.size() * req.model->T_grid.size());
        const auto np = static_cast<std::size_t>(n_params);
        const uint64_t reps = replicates_of(req);
        const uint64_t bps = LB::count(req.n_units);

        // Each device: its blocks' price statistics and warp gradient sums.
        auto job = [&](int device, uint64_t first, uint64_t last)
        {
            DeviceArrays mem;
            const mc::ScriptPathView v = device_view(req, mem, device);
            const detail::StackLimitRestore release_stack;
            const long trail_len = mc::lv_trail_bound(*req.program, v.n_steps);

            // Launch size from the memory a thread needs (its trail, its
            // gradient row, a Sobol point), never a default; the warp
            // partials on top.
            const std::size_t per_block =
                static_cast<std::size_t>(LB::kThreadsPerBlock) * static_cast<std::size_t>(trail_len + n_params) *
                    sizeof(double) +
                static_cast<std::size_t>(LB::kWarpsPerBlock) * np * sizeof(double);
            const DrawSetup s = draw_setup(req, v, mem, device, per_block);
            const uint64_t per_launch = std::min<uint64_t>(s.blocks_per_launch, std::max<uint64_t>(last - first, 1));
            const long rows = static_cast<long>(per_launch) * LB::kThreadsPerBlock;
            const long max_warps = static_cast<long>(per_launch) * LB::kWarpsPerBlock;
            double *d_trail = mem.alloc<double>(static_cast<std::size_t>(rows) * static_cast<std::size_t>(trail_len));
            double *d_grad = mem.alloc<double>(static_cast<std::size_t>(rows) * np);
            double *d_warps = mem.alloc<double>(static_cast<std::size_t>(max_warps) * np);
            detail::check(cudaMemset(d_grad, 0, static_cast<std::size_t>(rows) * np * sizeof(double)), "cudaMemset");
            const AdjointUnit unit{v, s.draws, d_trail, d_grad, rows, req.n_units};

            std::vector<AdjointBlock> blocks(static_cast<std::size_t>(last - first));
            std::vector<double> host(static_cast<std::size_t>(max_warps) * np);
            auto after = [&](uint64_t b0, uint64_t count)
            {
                const long n_warps = static_cast<long>(count) * LB::kWarpsPerBlock;
                const long total = n_warps * n_params;
                fold_warps<<<static_cast<unsigned>((total + 255) / 256), 256>>>(d_grad, rows, n_params, n_warps,
                                                                                d_warps);
                detail::check(cudaGetLastError(), "fold_warps launch");
                detail::check(cudaMemcpy(host.data(), d_warps, static_cast<std::size_t>(total) * sizeof(double),
                                         cudaMemcpyDeviceToHost),
                              "cudaMemcpy");
                detail::check(cudaMemset(d_grad, 0, static_cast<std::size_t>(rows) * np * sizeof(double)),
                              "cudaMemset");
                for (uint64_t c = 0; c < count; ++c)
                {
                    std::vector<double> &ws = blocks[static_cast<std::size_t>(b0 + c - first)].warp_sums;
                    ws.resize(static_cast<std::size_t>(LB::kWarpsPerBlock) * np);
                    for (int w = 0; w < LB::kWarpsPerBlock; ++w)
                        for (std::size_t p = 0; p < np; ++p)
                            ws[static_cast<std::size_t>(w) * np + p] =
                                host[p * static_cast<std::size_t>(n_warps) +
                                     static_cast<std::size_t>(c) * LB::kWarpsPerBlock + static_cast<std::size_t>(w)];
                }
            };
            const std::vector<WelfordAccumulator> price =
                detail::run_block_range<WelfordAccumulator>(device, first, last, bps, req.n_units, unit, per_launch,
                                                            after);
            for (std::size_t b = 0; b < blocks.size(); ++b)
                blocks[b].price = price[b];
            return blocks;
        };
        const std::vector<AdjointBlock> blocks = detail::run_on_devices<AdjointBlock>(devices_of(req), bps * reps, job);

        // The host folds in global block order -- the same sums whatever the
        // number of devices. Pseudo-random: batches of one warp (512 paths),
        // the risks' mean the total over the paths, their error the spread
        // of the batch means (ratio estimator: tail batches may be shorter),
        // sums shifted by the first batch's mean against cancellation.
        // RQMC: one sum per replicate, the error the replicates' spread.
        const bool rqmc = reps > 1;
        std::vector<double> shift(np, 0.0), s1(shift), s2(shift), s1n(shift);
        std::vector<double> rep_sum(static_cast<std::size_t>(reps) * np, 0.0);
        double total_units = 0.0, sum_n2 = 0.0;
        long long batches = 0;
        for (uint64_t g = 0; g < blocks.size(); ++g)
            for (int w = 0; w < LB::kWarpsPerBlock; ++w)
            {
                const long long nb = warp_units(g % bps, w, req.n_units);
                if (nb == 0)
                    continue;
                const double *sum = &blocks[static_cast<std::size_t>(g)].warp_sums[static_cast<std::size_t>(w) * np];
                if (rqmc)
                {
                    double *r = &rep_sum[static_cast<std::size_t>(g / bps) * np];
                    for (std::size_t p = 0; p < np; ++p)
                        r[p] += sum[p];
                    continue;
                }
                const double n = static_cast<double>(nb);
                if (batches == 0)
                    for (std::size_t p = 0; p < np; ++p)
                        shift[p] = sum[p] / n;
                for (std::size_t p = 0; p < np; ++p)
                {
                    const double y = sum[p] / n - shift[p];
                    s1[p] += n * y;
                    s1n[p] += n * n * y;
                    s2[p] += n * n * y * y;
                }
                total_units += n;
                sum_n2 += n * n;
                ++batches;
            }

        ScriptAdjointGpuResult res;
        res.risks.assign(np, 0.0);
        res.risk_std_errors.assign(np, 0.0);
        std::vector<WelfordAccumulator> block_price;
        for (const AdjointBlock &b : blocks)
            block_price.push_back(b.price);
        const std::vector<WelfordAccumulator> price = detail::merge_segments(block_price, reps, bps);
        if (rqmc)
        {
            for (const WelfordAccumulator &p : price)
                res.price.add(p.mean); // replicate means: its std_error() is RQMC's
            for (std::size_t p = 0; p < np; ++p)
            {
                WelfordAccumulator acc;
                for (uint64_t b = 0; b < reps; ++b)
                    acc.add(rep_sum[static_cast<std::size_t>(b) * np + p] / static_cast<double>(req.n_units));
                res.risks[p] = acc.mean;
                res.risk_std_errors[p] = acc.std_error();
            }
            res.batches = static_cast<long long>(reps);
            return res;
        }
        res.price = price.front();
        res.batches = batches;
        for (std::size_t p = 0; p < np; ++p)
        {
            const double mu = total_units > 0.0 ? s1[p] / total_units : 0.0;
            res.risks[p] = shift[p] + mu;
            if (batches > 1)
            {
                const double b = static_cast<double>(batches);
                const double ss = s2[p] - 2.0 * mu * s1n[p] + mu * mu * sum_n2; // sum n^2 (ybar - mu)^2
                res.risk_std_errors[p] = std::sqrt(std::max(0.0, ss) * b / (b - 1.0)) / total_units;
            }
        }
        return res;
    }

} // namespace quantModeling::gpu
