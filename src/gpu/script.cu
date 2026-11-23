#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/sobol_bridge.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/scripting/cuda_codegen.hpp"
#include "quantModeling/utils/dual.hpp"

#include "logical_blocks.cuh"
#include "script_jit.hpp"
#include "script_units.cuh"

namespace quantModeling::gpu
{

    namespace
    {
        using L = ScriptLimits;
        using LB = mc::LogicalBlocks;
        using detail::ScriptUnit;
        using detail::UnitDraws;

        /**
         * @brief Device copies of host arrays, freed together.
         *
         * put() packs the arrays into one device chunk (each 256-byte
         * aligned) and returns their device address at once; the bytes are
         * staged on the host and reach the card on flush(), one copy for
         * them all. A request's ~26 arrays used to cost 26 allocations and
         * 26 copies (issue #330). **flush() before any kernel reads them**:
         * device_view, draw_setup and path_unit -- the only callers of put()
         * -- flush before their own launches and before returning.
         */
        struct DeviceArrays
        {
            std::vector<void *> owned;
            DeviceArrays() = default;
            DeviceArrays(const DeviceArrays &) = delete;
            DeviceArrays &operator=(const DeviceArrays &) = delete;
            ~DeviceArrays()
            {
                for (void *p : owned)
                    detail::device_free(p);
            }
            template <class T>
            const T *put(const std::vector<T> &h)
            {
                if (h.empty())
                    return nullptr;
                const std::size_t bytes = h.size() * sizeof(T);
                std::size_t at = (used_ + kAlign - 1) / kAlign * kAlign;
                if (!chunk_ || at + bytes > capacity_)
                {
                    flush();
                    capacity_ = std::max(kChunkBytes, bytes);
                    chunk_ = detail::device_alloc<unsigned char>(capacity_);
                    owned.push_back(chunk_);
                    used_ = flushed_ = at = 0;
                }
                staged_.resize(at + bytes - flushed_); // the padding staged as zeros
                std::memcpy(staged_.data() + (at - flushed_), h.data(), bytes);
                used_ = at + bytes;
                return reinterpret_cast<const T *>(chunk_ + at);
            }
            /// Copies what put() staged since the last flush to the device.
            void flush()
            {
                if (used_ > flushed_)
                    detail::check(cudaMemcpy(chunk_ + flushed_, staged_.data(), used_ - flushed_,
                                             cudaMemcpyHostToDevice),
                                  "cudaMemcpy");
                flushed_ = used_;
                staged_.clear();
            }
            template <class T>
            T *alloc(std::size_t n)
            {
                T *d = detail::device_alloc<T>(n);
                owned.push_back(d);
                return d;
            }

          private:
            static constexpr std::size_t kAlign = 256;
            /// A chunk holds a request's small arrays; a larger array gets a
            /// chunk of its own size.
            static constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
            unsigned char *chunk_ = nullptr;
            std::size_t capacity_ = 0, used_ = 0, flushed_ = 0;
            std::vector<unsigned char> staged_; ///< bytes [flushed_, used_) of the chunk
        };

        /**
         * The path-independent factors of each event, once instead of on every
         * path (lot P1i of blueprint/wp/26-performance.md): exp(r t) for the
         * numeraire and exp(-r (T - t)) for each df(), with script_path's
         * expressions and the same device exp -- the same bits. Measured on
         * one V100 (ncu, 2026-11-11): see the commit and WP 26 §5, P1i.
         */
        __global__ void event_tables_kernel(const Time *t, const int *event, int n_steps, double r,
                                            const int *disc_begin, const Time *disc_mats, double *numeraire,
                                            double *disc)
        {
            const int s = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
            if (s >= n_steps)
                return;
            const int e = event[s];
            if (e < 0)
                return;
            const Time ts = t[s];
            for (int i = disc_begin[e]; i < disc_begin[e + 1]; ++i)
                disc[i] = exp(-r * (disc_mats[i] - ts));
            numeraire[e] = exp(r * ts);
        }

        /// The same factors as dense rows, for the generated paths (WP 27 lot
        /// L2): row e = [exp(r t_e), exp(-r (T_i - t_e)) per df() slot, then
        /// exp((r - q) (T_j - t_e)) per fwd() slot from column fwd_col0, q
        /// spot(0)'s dividend yield -- what ir_path.cpp's path_data writes,
        /// with the device's exp.
        __global__ void event_rows_kernel(const Time *t, const int *event, int n_steps, double r,
                                          const int *disc_begin, const Time *disc_mats, int cols, double *rows,
                                          double carry, const int *fwd_begin, const Time *fwd_mats, int fwd_col0)
        {
            const int s = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
            if (s >= n_steps)
                return;
            const int e = event[s];
            if (e < 0)
                return;
            const Time ts = t[s];
            double *row = rows + static_cast<long>(e) * cols;
            for (int i = disc_begin[e]; i < disc_begin[e + 1]; ++i)
                row[1 + i - disc_begin[e]] = exp(-r * (disc_mats[i] - ts));
            if (fwd_begin)
                for (int i = fwd_begin[e]; i < fwd_begin[e + 1]; ++i)
                    row[fwd_col0 + i - fwd_begin[e]] = exp(carry * (fwd_mats[i] - ts));
            row[0] = exp(r * ts);
        }

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
            v.variance_scheme = dm.variance_scheme;
            v.disc_begin = mem.put(disc_begin);
            v.disc_mats = mem.put(disc_mats);
            v.controls = req.controls;

            // The per-event tables (prices; the duals differentiate r and
            // compute them on the path).
            if (v.n_steps > 0 && p.n_events() > 0)
            {
                double *num = mem.alloc<double>(p.n_events());
                double *disc = disc_mats.empty() ? nullptr : mem.alloc<double>(disc_mats.size());
                mem.flush(); // the kernel reads t, event and the discount lists
                const unsigned threads = 256;
                event_tables_kernel<<<static_cast<unsigned>((v.n_steps + threads - 1) / threads), threads>>>(
                    v.t, v.event, v.n_steps, v.r, v.disc_begin, v.disc_mats, num, disc);
                detail::check(cudaGetLastError(), "event tables launch");
                v.event_numeraire = num;
                v.event_disc = disc;
            }
            mem.flush();
            return v;
        }

        /// What a request's draws need on the device; `blocks_per_launch`
        /// comes back capped by the Sobol scratch the memory allows.
        struct DrawSetup
        {
            UnitDraws draws;
            uint64_t replicates = 1;
            uint64_t blocks_per_launch = 0;
        };

        /// `parallel_units`: one thread per unit (lot P1g), so a logical
        /// block's Sobol scratch is 2 048 rows, not 256.
        DrawSetup draw_setup(const ScriptGpuRequest &req, const mc::ScriptPathView &v, DeviceArrays &mem, int device,
                             std::size_t other_bytes_per_block = 0, bool parallel_units = false)
        {
            const std::size_t threads_per_block =
                static_cast<std::size_t>(parallel_units ? LB::kUnitsPerBlock : LB::kThreadsPerBlock);
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
                sv.pca = t.pca.empty() ? nullptr : mem.put(t.pca);
                s.replicates = static_cast<uint64_t>(std::max(1, t.replicates));
                per_block += threads_per_block * 2u * static_cast<std::size_t>(t.dim) * sizeof(double);
            }
            if (per_block > 0)
            {
                // Launch size from the memory a thread needs, never a default:
                // at most half of what is free (the cards are shared).
                const uint64_t n_blocks = LB::count(req.n_units) * s.replicates;
                const std::size_t budget = detail::launch_budget(device, std::max<uint64_t>(n_blocks, 1) * per_block);
                uint64_t per_launch = std::max<uint64_t>(1, budget / per_block);
                // And at most kScratchBytes: one thread per unit (P1g) made a
                // Sobol launch's scratch 4 096 rows a block (2 048 since 2026-11-16) -- 4.8 GB for the
                // daily Asian's 1.16 M paths, whose allocation after the last
                // call's free cost 89 ms of a 131 ms call (one V100,
                // 2026-11-11). 1 GiB is still 60 blocks of 4 096 threads at
                // 256 dimensions, more than the card holds at once; the
                // result does not depend on the launch size.
                constexpr std::size_t kScratchBytes = std::size_t{1} << 30;
                // Unless the whole request fits one launch within the budget
                // and 4 GiB: below one wave a launch takes as long whatever
                // its block count (each thread's units run in sequence), so
                // the cap's remainder doubled the time -- SLV's adjoint (a
                // gradient row of 12 908 parameters per thread, 34 MB a block)
                // ran 31 blocks, then 1 alone, 52 + 46 ms on one V100 for the
                // phoenix autocall (lot B of #331, 2026-11-19).
                const bool one_launch = n_blocks <= per_launch && n_blocks * per_block <= 4 * kScratchBytes;
                if (!one_launch)
                    per_launch = std::min<uint64_t>(per_launch, std::max<uint64_t>(1, kScratchBytes / per_block));
                if (s.blocks_per_launch > 0)
                    per_launch = std::min(per_launch, s.blocks_per_launch);
                s.blocks_per_launch = std::min<uint64_t>(per_launch, std::max<uint64_t>(n_blocks, 1));
            }
            if (req.sobol)
            {
                s.draws.rows = static_cast<long>(s.blocks_per_launch * threads_per_block);
                s.draws.scratch = mem.alloc<double>(static_cast<std::size_t>(s.draws.rows) * 2u *
                                                    static_cast<std::size_t>(req.sobol->dim));
            }
            mem.flush();
            return s;
        }

        // ── prices ──────────────────────────────────────────────────────────

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

        /// Where the fold's rows keep each risk: the scalar parameters
        /// first, as listed, then the grid time-major -- node (i, j) at
        /// grid0 + j nK + i, where the risks list it at grid0 + i nT + j.
        struct GridOrder
        {
            int grid0 = 0, nK = 1, nT = 1; ///< nK = nT = 1: no grid

            __device__ int slot(int p) const
            {
                if (p < grid0)
                    return p;
                const int g = p - grid0;
                return grid0 + (g % nT) * nK + g / nT;
            }
            __device__ int param(int q) const
            {
                if (q < grid0)
                    return q;
                const int g = q - grid0;
                return grid0 + (g % nK) * nT + g / nK;
            }
        };

        /**
         * The adjoint's sparse trails (issue #342), first pass: the 32 trails
         * of each CUDA warp `c` of the launch -- units 32 c to 32 c + 31 of
         * the launch, one thread per unit -- summed into the warp's dense
         * row, warp_rows[c * n_params + p].
         *
         * One warp per row, no atomics: lane `lane` reads the trail of
         * thread 32 c + lane, pair e of the 32 trails at once (coalesced:
         * pair e of thread t is at e * rows + t). Lanes on the same
         * parameter are summed in lane order and the lowest of them adds the
         * sum to the row: a node's sum is taken in the order e, then lane,
         * fixed by the units
         * whatever the launch size or the number of devices. `overflow` is
         * set when a trail is longer than its arrays, which the IR's bound
         * makes impossible (a bug, not a user error).
         *
         * The row holds the grid time-major (GridOrder): at a given step the
         * warp's 32 paths reach nodes of the same time column, which are then
         * neighbours in the row. Strike-major, as the risks are listed, they
         * were nT doubles apart and the 2 048 rows of the phoenix autocall
         * under SLV missed L2 half the time (fold 27 ms; 65 536 paths on the
         * page's 100 x 129 grid, one V100, 2026-11-20); time-major, 7.7 ms.
         *
         * The lanes' sums take 32 uniform shuffles, every lane adding those
         * that share its parameter: __match_any_sync's groups, each summed
         * by a loop of its own, diverged -- the groups ran one after the
         * other, and paths collide often (7.7 -> 5.0 ms under SLV, 17.1 ->
         * 7.0 ms a launch under local vol).
         */
        __global__ void fold_trails(const std::int32_t *index, const double *value, const std::int32_t *count,
                                    long rows, int cap, int n_params, GridOrder order, long n_warps,
                                    double *warp_rows, int *overflow)
        {
            constexpr unsigned kAll = 0xffffffffu;
            const long c = (static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x) / LB::kWarpSize;
            const int lane = static_cast<int>(threadIdx.x % LB::kWarpSize);
            if (c >= n_warps) // whole warps: blockDim is a multiple of 32
                return;
            double *row = warp_rows + c * n_params;
            for (int p = lane; p < n_params; p += LB::kWarpSize)
                row[p] = 0.0;
            __syncwarp();
            const long t = c * LB::kWarpSize + lane;
            int n = count[t];
            if (n > cap)
            {
                atomicExch(overflow, 1);
                n = cap;
            }
            // pair e + 1 is loaded while pair e is added
            std::int32_t p = n > 0 ? index[t] : -1;
            double v = n > 0 ? value[t] : 0.0;
            for (int e = 0; __any_sync(kAll, e < n); ++e)
            {
                const bool next = e + 1 < n;
                const std::int32_t p1 = next ? index[static_cast<long>(e + 1) * rows + t] : -1;
                const double v1 = next ? value[static_cast<long>(e + 1) * rows + t] : 0.0;
                double sum = 0.0;
                int lead = -1;
                for (int j = 0; j < LB::kWarpSize; ++j)
                {
                    const std::int32_t pj = __shfl_sync(kAll, p, j);
                    const double vj = __shfl_sync(kAll, v, j);
                    if (pj == p)
                    {
                        sum += vj;
                        lead = lead < 0 ? j : lead;
                    }
                }
                if (p >= 0 && lane == lead)
                    row[order.slot(p)] += sum;
                p = p1;
                v = v1;
            }
        }

        /**
         * Second pass: the rows of the batches the risks' errors are taken
         * over -- warp w of logical block b, units w * 32 + lane + 256 k,
         * k = 0..7, i.e. CUDA warps 8 k + w of the block -- in the order k:
         * out[(8 b + w) * n_params + p]. With the first pass, a node's sum
         * is taken in the order k, e, lane. The dense rows summed each
         * thread's 8 paths first: another order, within the CPU/GPU
         * tolerance (2026-11-16). Read in the rows' order, written in the
         * risks'.
         */
        __global__ void fold_batches(const double *warp_rows, int n_params, GridOrder order, long n_batches,
                                     double *out)
        {
            const long i = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (i >= n_batches * n_params)
                return;
            const long batch = i / n_params, q = i % n_params;
            const long block = batch / LB::kWarpsPerBlock, w = batch % LB::kWarpsPerBlock;
            const double *first = warp_rows + (block * LB::kThreadsPerBlock * LB::kUnitsPerThread / LB::kWarpSize + w) * n_params +
                                  q;
            double sum = 0.0;
            for (int k = 0; k < LB::kUnitsPerThread; ++k)
                sum += first[static_cast<long>(k) * LB::kWarpsPerBlock * n_params];
            out[batch * n_params + order.param(q)] = sum;
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
                const DrawSetup s = draw_setup(req, v, mem, device, 0, req.parallel_units);
                const auto unit = make_unit(v, s);
                if (!req.parallel_units)
                    return detail::run_block_range<Stats>(device, first, last, bps, req.n_units, unit,
                                                          s.blocks_per_launch);
                return detail::run_block_range_parallel<Stats>(device, first, last, bps, req.n_units, unit,
                                                               s.blocks_per_launch);
            };
            return detail::merge_segments(detail::run_on_devices<Stats>(devices_of(req), bps * reps, job), reps, bps);
        }

        using scripting::PathKernel;

        /// The request's data on the device, as the generated paths read it
        /// (scripting::ir::PathData), and the unit their kernels take.
        detail::PathUnit path_unit(const ScriptGpuRequest &req, DeviceArrays &mem, const mc::ScriptPathView &v,
                                   const UnitDraws &draws, bool risk)
        {
            namespace ir = scripting::ir;
            const ir::PathLayout l = ir::path_layout(req.model->kind, req.model->n_assets, *req.ir, risk);
            const ir::PathData data = ir::path_data(l, *req.model, *req.ir, req.discount_mats, req.baseline);
            detail::PathUnit u;
            u.draws = draws;
            u.antithetic = req.antithetic;
            u.per_replicate = req.n_units;
            u.theta = mem.put(data.theta);
            u.step_rows = mem.put(data.step_rows);
            u.baseline = mem.put(data.baseline);
            u.terms = mem.put(data.terms);
            u.term_begin = mem.put(data.term_begin);
            u.kind = mem.put(req.ir->event_block);
            u.step_draws = v.draws;
            u.step_event = v.event;
            u.n_steps = v.n_steps;
            u.grid_K = v.grid.K;
            u.grid_T = v.grid.T_grid;
            u.grid_nK = v.grid.nK;
            u.grid_nT = v.grid.nT;
            u.grid0 = l.th_grid;
            u.controls = req.controls;
            if (risk || data.event_rows.empty() || v.n_steps == 0)
                u.event_rows = mem.put(data.event_rows); // times and maturities: the blocks take their exp
            else
            {
                // The price's factors, tabulated with the device's exp: the
                // interpreter's bits (WP 26 P1i).
                double *rows = mem.alloc<double>(data.event_rows.size());
                // fwd(): its maturities per event, from the IR
                const int *fwd_begin = nullptr;
                const Time *fwd_mats = nullptr;
                if (req.ir->has_forward)
                {
                    std::vector<int> begin{0};
                    std::vector<Time> mats;
                    for (const auto &m : req.ir->event_fwd_mats)
                    {
                        mats.insert(mats.end(), m.begin(), m.end());
                        begin.push_back(static_cast<int>(mats.size()));
                    }
                    fwd_begin = mem.put(begin);
                    fwd_mats = mem.put(mats);
                }
                const DeviceModel &dm = *req.model;
                const double carry = dm.r - (dm.n_assets > 1 ? dm.divs[0] : dm.q);
                mem.flush(); // the kernel reads the view's tables and fwd()'s maturities
                const unsigned threads = 256;
                event_rows_kernel<<<static_cast<unsigned>((v.n_steps + threads - 1) / threads), threads>>>(
                    v.t, v.event, v.n_steps, v.r, v.disc_begin, v.disc_mats, l.event_cols, rows, carry, fwd_begin,
                    fwd_mats, l.fwd_col0);
                detail::check(cudaGetLastError(), "event rows launch");
                u.event_rows = rows;
            }
            mem.flush();
            return u;
        }

        /// A request's compiled kernel on each of its devices.
        struct CompiledKernels
        {
            std::vector<void *> fns; ///< empty: not compiled, `note` says why
            std::string note;
            /// The adjoint's bounds on a path's trail (CudaSource::grid_writes_*).
            int grid_writes_init = 0, grid_writes_step = 0, grid_writes_event = 0;
            bool ok() const { return !fns.empty(); }
            void *on(const ScriptGpuRequest &req, int device) const
            {
                const std::vector<int> devices = devices_of(req);
                return fns[static_cast<std::size_t>(std::find(devices.begin(), devices.end(), device) -
                                                    devices.begin())];
            }
        };

        /// The kernel `name` of `req`'s generated path, compiled (or found in
        /// the caches) for `kernel`; without it, why the interpreter runs.
        CompiledKernels compiled_kernels(const ScriptGpuRequest &req, PathKernel kernel, const char *name)
        {
            CompiledKernels out;
            const auto refuse = [&](const std::string &why)
            {
                // The interpreted kernels (and the hand-written adjoint and
                // duals) read no last-step expectation: only the kernels
                // compiled from the IR compute them (WP 27 §14.9).
                if (req.program && req.program->has_monitor)
                    throw InvalidInput("GPU: lastStep*() and survivalAbove() / survivalBelow() need the kernels "
                                       "compiled from the IR, and they are not available here (" +
                                       why + ")");
                if (req.program && req.program->has_exercise)
                    throw InvalidInput("GPU: exercise() / call() need the kernels compiled from the IR, and they "
                                       "are not available here (" +
                                       why + ")");
                if (req.program && req.program->has_forward)
                    throw InvalidInput("GPU: fwd() needs the kernels compiled from the IR, and they are not "
                                       "available here (" +
                                       why + ")");
                out.note = "interpreted bytecode (" + why + ")";
                return out;
            };
            if (!req.compile)
                return refuse("compilation off");
            if (!jit::enabled())
                return refuse("QM_SCRIPT_JIT=0");
            if (kernel == PathKernel::Price && !req.parallel_units)
                return refuse("serial units");
            if (!req.ir || req.ir->empty())
                return refuse("the request carries no IR");
            const scripting::CudaSource src = scripting::cuda_path_source(
                *req.ir, static_cast<int>(req.model->kind), req.model->n_assets, kernel,
                req.model->variance_scheme == mc::VarianceScheme::QE);
            if (src.source.empty())
                return refuse(src.why);
            scripting::CudaKernelSpec spec;
            spec.model_kind = static_cast<int>(req.model->kind);
            spec.n_assets = req.model->n_assets;
            spec.sobol = req.sobol != nullptr;
            spec.stratified = req.stratified;
            spec.importance = !req.is_theta.empty();
            spec.antithetic = kernel == PathKernel::Price && req.antithetic;
            spec.controls = kernel == PathKernel::Price ? req.controls.n : 0;
            const std::vector<int> devices = devices_of(req);
            std::string why;
            bool cached = false;
            // Vibrato's blocks link with the tangent's kernel (its driver is there).
            const int fatbin = kernel == PathKernel::Vibrato ? static_cast<int>(PathKernel::Tangent)
                                                             : static_cast<int>(kernel);
            const auto built = jit::compile(src.source + scripting::cuda_spec_source(spec), fatbin, devices.front(),
                                            why, &cached);
            if (!built)
                return refuse(why);
            for (int device : devices)
            {
                void *fn = jit::kernel(built, device, name, why);
                if (!fn)
                {
                    out.fns.clear();
                    return refuse(why);
                }
                out.fns.push_back(fn);
            }
            char ms[96];
            if (built->from_disk)
                std::snprintf(ms, sizeof ms, "read from the kernel cache in %.1f ms", built->compile_ms);
            else
                std::snprintf(ms, sizeof ms, "compiled in %.0f ms: NVRTC %.0f, link %.0f", built->compile_ms,
                              built->nvrtc_ms, built->link_ms);
            out.note = std::string("compiled script (NVRTC, ") + (cached ? "cached" : ms) + ")";
            out.grid_writes_init = src.grid_writes_init;
            out.grid_writes_step = src.grid_writes_step;
            out.grid_writes_event = src.grid_writes_event;
            return out;
        }

        /// The request's logical blocks on its compiled kernel, one thread
        /// per unit: the blocks, devices and reduction of run_script_blocks.
        /// `extra` is appended to the kernel's arguments.
        template <class Stats, class Out>
        std::vector<Stats> run_compiled_units(const ScriptGpuRequest &req, const CompiledKernels &k, bool risk,
                                              int out_stride = 0)
        {
            const std::vector<int> devices = devices_of(req);
            const uint64_t reps = replicates_of(req);
            const uint64_t bps = LB::count(req.n_units);
            auto job = [&](int device, uint64_t first, uint64_t last)
            {
                DeviceArrays mem;
                const mc::ScriptPathView v = device_view(req, mem, device);
                const detail::StackLimitRestore release_stack;
                const DrawSetup s = draw_setup(req, v, mem, device, 0, true);
                detail::PathUnit unit = path_unit(req, mem, v, s.draws, risk);
                void *fn = k.on(req, device);
                uint64_t n_units = req.n_units, per_segment = bps;
                int stride = out_stride;
                const detail::LaunchUnits<Out> launch = [&](uint64_t b, uint64_t count, Out *d_samples)
                {
                    void *args[] = {&b, &per_segment, &n_units, &count, &unit, &d_samples, &stride};
                    jit::launch(fn, detail::unit_grid(count), LB::kThreadsPerBlock, args);
                };
                return detail::run_block_range_units<Stats, Out>(device, first, last, bps, req.n_units,
                                                                 s.blocks_per_launch, launch);
            };
            return detail::merge_segments(detail::run_on_devices<Stats>(devices, bps * reps, job), reps, bps);
        }

        /// One slice of the adjoint's run, folded as its batches arrive
        /// (issue #348): the gradient sums of a batch are added to their
        /// slice's accumulators and dropped, so the host holds O(slices x
        /// risks), not O(paths x risks) -- 3.7 GB at 5M paths under SLV on
        /// the page's grid, which killed production (#347).
        ///
        /// The slices are fixed by the path count alone (kFoldSlices, never
        /// across a replicate), the cards share whole slices, and the slices
        /// merge in their order: the risks do not depend on the number of
        /// cards, bit for bit (ADR-G5).
        struct FoldSlice
        {
            std::vector<WelfordAccumulator> price; ///< per logical block, in order
            /// Pseudo-random: batch means shifted by the slice's first batch
            /// mean (against cancellation), sum n y, sum n^2 y, sum n^2 y^2.
            /// RQMC: s1 alone, the slice's share of its replicate's sum.
            std::vector<double> shift, s1, s1n, s2;
            double total_units = 0.0, sum_n2 = 0.0;
            long long batches = 0;
        };

        /// Slices of the fold: at most this many per run, at least one per
        /// replicate. 64 x 4 x 12,908 risks: 26 MB, whatever the path count.
        constexpr uint64_t kFoldSlices = 64;
        /// ... of at least this many logical blocks (65k paths): the page's
        /// 50,000 paths fold into one slice, as fast as the single buffer
        /// was (16 slices cost it 2-3 ms, zeroing their sums).
        constexpr uint64_t kFoldMinSliceBlocks = 16;
        /// Host bytes of one launch's batch rows, copied then folded: the
        /// launch is cut to fit (77 logical blocks, 315k paths, at 12,908 risks).
        constexpr std::size_t kFoldStagingBytes = std::size_t{64} << 20;
    } // namespace

    void forget_compiled_scripts()
    {
        jit::forget_compiled();
    }

    ScriptKernelsRebuilt rebuild_script_kernels(int device)
    {
        detail::require_device(device);
        const jit::Rebuilt r = jit::rebuild_stale(device);
        return {r.current, r.rebuilt, r.failed};
    }

    std::string script_kernel_cache_dir()
    {
        return jit::disk_cache_dir();
    }

    ScriptGpuStats simulate_script(const ScriptGpuRequest &req)
    {
        ScriptGpuStats out;
        if (!req.program || !req.model)
            throw InvalidInput("simulate_script: program and model are required");
        const bool controlled = req.controls.n > 0;
        const CompiledKernels k =
            compiled_kernels(req, PathKernel::Price,
                             controlled ? "qm_script_controlled"
                             : req.model->kind == DeviceModel::Kind::SLV ||
                                     req.model->kind == DeviceModel::Kind::Heston
                                 ? "qm_script_plain_r64"
                                 : "qm_script_plain");
        out.events_note = k.note;
        out.compiled = k.ok();
        using Sample = MultiControlAccumulator::Sample;
        const auto interpreted = [&](const mc::ScriptPathView &v, const DrawSetup &s)
        {
            return ScriptUnit<true>{v, s.draws, req.antithetic, req.n_units,
                                    mc::InterpretedEvents{v.prog, v.event_begin}};
        };
        if (controlled && req.stratified)
            out.stratified_controlled =
                k.ok() ? run_compiled_units<StratifiedControlAccumulator, Sample>(req, k, false)
                       : run_script_blocks<StratifiedControlAccumulator>(req, interpreted);
        else if (controlled)
            out.controlled = k.ok() ? run_compiled_units<MultiControlAccumulator, Sample>(req, k, false)
                                    : run_script_blocks<MultiControlAccumulator>(req, interpreted);
        else if (k.ok())
            out.plain = run_compiled_units<WelfordAccumulator, double>(req, k, false);
        else
            out.plain = run_script_blocks<WelfordAccumulator>(
                req, [&](const mc::ScriptPathView &v, const DrawSetup &s)
                { return ScriptUnit<false>{v, s.draws, req.antithetic, req.n_units, mc::InterpretedEvents{v.prog, v.event_begin}}; });
        return out;
    }

    namespace
    {
        /// The compiled tangent kernel's samples as Dual<N>, reduced like the
        /// interpreter's duals.
        template <int N>
        std::vector<std::vector<WelfordAccumulator>> run_compiled_tangent(const ScriptGpuRequest &req,
                                                                          const CompiledKernels &k, int dirs)
        {
            const auto st = run_compiled_units<DualStats<N>, Dual<N>>(req, k, true, N + 1);
            std::vector<std::vector<WelfordAccumulator>> out;
            for (const DualStats<N> &r : st)
                out.emplace_back(r.w, r.w + 1 + dirs);
            return out;
        }
    } // namespace

    std::vector<std::vector<WelfordAccumulator>> simulate_script_duals(const ScriptGpuRequest &req, std::string *note)
    {
        if (!req.model)
            throw InvalidInput("simulate_script: program and model are required");
        const int dirs = mc::dual_directions(req.model->kind, req.model->n_assets);
        if (dirs <= 0)
            throw InvalidInput("GPU: forward-mode risks cover Black-Scholes and Heston");
        // The tangent built from the IR (WP 27 lot L3): any number of
        // directions. The dual numbers on the interpreter, its oracle until
        // the tangent beat them, are gone (maintainer's decision 2026-11-16):
        // without compilation the risks take the CPU tape.
        ScriptGpuRequest unmirrored = req;
        unmirrored.antithetic = false;
        const CompiledKernels k = compiled_kernels(unmirrored, PathKernel::Tangent, "qm_script_tangent");
        if (!k.ok())
            throw InvalidInput("GPU: forward-mode risks run in the kernels compiled from the IR (" + k.note + ")");
        if (note)
            *note = "tangent kernel, " + k.note;
        return dirs <= 8 ? run_compiled_tangent<8>(unmirrored, k, dirs) : run_compiled_tangent<32>(unmirrored, k, dirs);
    }

    std::vector<std::vector<WelfordAccumulator>> simulate_script_vibrato(const ScriptGpuRequest &req, int dirs,
                                                                         std::string *note)
    {
        if (!req.model)
            throw InvalidInput("simulate_script: program and model are required");
        ScriptGpuRequest unmirrored = req;
        unmirrored.antithetic = false;
        const CompiledKernels k = compiled_kernels(unmirrored, PathKernel::Vibrato, "qm_script_tangent");
        if (!k.ok())
            throw InvalidInput("GPU: Vibrato runs in the kernels compiled from the IR (" + k.note + ")");
        if (note)
            *note = "Vibrato kernel, " + k.note;
        if (dirs > 8)
            throw InvalidInput("GPU: Vibrato covers one underlying (at most 8 risks)");
        return run_compiled_tangent<8>(unmirrored, k, dirs);
    }

    ScriptAdjointGpuResult simulate_script_adjoint(const ScriptGpuRequest &req)
    {
        if (!req.model || !req.program)
            throw InvalidInput("simulate_script: program and model are required");
        if (req.stratified || !req.is_theta.empty())
            throw InvalidInput("GPU: the per-path adjoint runs pseudo-random or Sobol paths");

        // The adjoint built from the IR (WP 27 lot L4), for every model whose
        // step is in the IR. The local-vol adjoint written by hand, its oracle
        // until it was beaten (§14.6), is gone (maintainer's decision
        // 2026-11-16): without compilation the risks take the CPU tape.
        ScriptGpuRequest unmirrored = req;
        unmirrored.antithetic = false;
        const CompiledKernels compiled = compiled_kernels(unmirrored, PathKernel::Adjoint, "qm_script_adjoint");
        if (!compiled.ok())
            throw InvalidInput("GPU: the per-path adjoint runs in the kernels compiled from the IR (" + compiled.note +
                               ")");
        const scripting::ir::PathLayout layout =
            scripting::ir::path_layout(req.model->kind, req.model->n_assets, *req.ir, true);
        // The risks: the model's scalar parameters, then its grid's points.
        const int n_params =
            layout.n_risks + (layout.th_grid >= 0 ? static_cast<int>(req.model->grid.size()) : 0);
        const auto np = static_cast<std::size_t>(n_params);
        const uint64_t reps = replicates_of(req);
        const uint64_t bps = LB::count(req.n_units);
        const bool rqmc = reps > 1;
        // The fold's slices: per_rep in each replicate, slice c covering the
        // blocks [slice_begin(c), slice_begin(c + 1)).
        const uint64_t per_rep =
            std::clamp<uint64_t>(kFoldSlices / reps, 1, std::max<uint64_t>(bps / kFoldMinSliceBlocks, 1));
        const uint64_t n_slices = reps * per_rep;
        const auto slice_begin = [&](uint64_t c)
        { return (c / per_rep) * bps + bps * (c % per_rep) / per_rep; };

        // Each device: its slices, folded as their batches arrive.
        auto job = [&](int device, uint64_t c0, uint64_t c1)
        {
            DeviceArrays mem;
            const mc::ScriptPathView v = device_view(req, mem, device);
            const detail::StackLimitRestore release_stack;
            // Per thread: the compiled adjoint's checkpoints (the model's
            // state per drawing step, the script's variables and the payoff
            // per date).
            long trail_len = 0;
            {
                long drawing = 0;
                for (int d : req.model->draws)
                    drawing += d ? 1 : 0;
                // per date, what qm_drive saves: every slot past the
                // model's (the variables, the payoff, and the last step's
                // forward and variance when the script reads them)
                // per drawing step: the model's state and the survivals'
                // running products
                trail_len = drawing * (layout.n_model + layout.n_surv) +
                            static_cast<long>(req.program->n_events()) * (layout.n_state() - layout.n_model);
            }
            trail_len = std::max<long>(trail_len, 1);
            // and its gradient trail's pairs, at most: every statement of a
            // block runs at most once a run (the IR has no loops), so the
            // bound holds by construction (issue #342)
            long cap = 0;
            {
                long drawing = 0;
                for (int d : req.model->draws)
                    drawing += d ? 1 : 0;
                cap = drawing * compiled.grid_writes_step +
                      static_cast<long>(req.program->n_events()) * compiled.grid_writes_event +
                      compiled.grid_writes_init + layout.n_risks;
            }
            if (cap > INT32_MAX / 2)
                throw InvalidInput("GPU: the per-path adjoint's gradient trail would be too long");
            cap = std::max<long>(cap, 1);

            // Launch size from the memory a unit needs (its checkpoints, its
            // trail, a Sobol point), never a default; each batch's dense row
            // on top. Phoenix autocall under SLV on the page's 100 x 129
            // leverage grid (65 536 paths, 504 steps): ~21 KB a path in a
            // single launch, where the dense row took 100 KB a thread at 8
            // paths a thread.
            const std::size_t per_unit = static_cast<std::size_t>(trail_len) * sizeof(double) +
                                         static_cast<std::size_t>(cap) * (sizeof(std::int32_t) + sizeof(double)) +
                                         sizeof(std::int32_t);
            constexpr std::size_t kUnitWarps = LB::kUnitsPerBlock / LB::kWarpSize; // 64 a logical block
            const std::size_t per_block = static_cast<std::size_t>(LB::kUnitsPerBlock) * per_unit +
                                          (kUnitWarps + LB::kWarpsPerBlock) * np * sizeof(double);
            const DrawSetup s = draw_setup(req, v, mem, device, per_block, true);
            const uint64_t span = slice_begin(c1) - slice_begin(c0);
            const uint64_t per_launch = std::min<uint64_t>(
                {s.blocks_per_launch, std::max<uint64_t>(span, 1),
                 std::max<uint64_t>(1, kFoldStagingBytes / (static_cast<std::size_t>(LB::kWarpsPerBlock) * np *
                                                            sizeof(double)))});
            const long rows = static_cast<long>(per_launch) * LB::kUnitsPerBlock;
            const long max_batches = static_cast<long>(per_launch) * LB::kWarpsPerBlock;
            const auto urows = static_cast<std::size_t>(rows);
            double *d_trail = mem.alloc<double>(urows * static_cast<std::size_t>(trail_len));
            auto *d_index = mem.alloc<std::int32_t>(urows * static_cast<std::size_t>(cap));
            double *d_value = mem.alloc<double>(urows * static_cast<std::size_t>(cap));
            auto *d_count = mem.alloc<std::int32_t>(urows);
            double *d_warp_rows = mem.alloc<double>(static_cast<std::size_t>(per_launch) * kUnitWarps * np);
            double *d_rows = mem.alloc<double>(static_cast<std::size_t>(max_batches) * np);
            int *d_overflow = mem.alloc<int>(1);
            detail::check(cudaMemset(d_overflow, 0, sizeof(int)), "cudaMemset");
            detail::PathUnit path = path_unit(unmirrored, mem, v, s.draws, true);
            path.checkpoints = d_trail;
            path.trail_index = d_index;
            path.trail_value = d_value;
            path.trail_count = d_count;
            path.trail_cap = static_cast<std::int32_t>(cap);
            path.rows = rows;
            GridOrder order;
            if (layout.th_grid >= 0 && !req.model->grid.empty() &&
                static_cast<long>(path.grid0) + static_cast<long>(path.grid_nK) * path.grid_nT == n_params)
            {
                order.grid0 = path.grid0;
                order.nK = path.grid_nK;
                order.nT = path.grid_nT;
            }

            // The slices [c0, c1) of this card, their blocks [first, last).
            const uint64_t first = slice_begin(c0), last = slice_begin(c1);
            std::vector<FoldSlice> slices(static_cast<std::size_t>(c1 - c0));
            for (FoldSlice &f : slices)
            {
                f.s1.assign(np, 0.0);
                if (!rqmc)
                {
                    f.shift.assign(np, 0.0);
                    f.s1n.assign(np, 0.0);
                    f.s2.assign(np, 0.0);
                }
            }
            // the slice a block belongs to: blocks arrive in increasing order
            uint64_t slice = c0;
            const std::size_t block_doubles = static_cast<std::size_t>(LB::kWarpsPerBlock) * np;
            // not value-initialised: each copy writes what is then read
            const std::unique_ptr<double[]> staging(new double[static_cast<std::size_t>(per_launch) * block_doubles]);
            auto after = [&](uint64_t b0, uint64_t count)
            {
                const long n_batches = static_cast<long>(count) * LB::kWarpsPerBlock;
                const long n_warps = static_cast<long>(count) * static_cast<long>(kUnitWarps);
                fold_trails<<<static_cast<unsigned>((n_warps * LB::kWarpSize + 255) / 256), 256>>>(
                    d_index, d_value, d_count, rows, static_cast<int>(cap), n_params, order, n_warps, d_warp_rows,
                    d_overflow);
                detail::check(cudaGetLastError(), "fold_trails launch");
                const long cells = n_batches * n_params;
                fold_batches<<<static_cast<unsigned>((cells + 255) / 256), 256>>>(d_warp_rows, n_params, order,
                                                                                  n_batches, d_rows);
                detail::check(cudaGetLastError(), "fold_batches launch");
                // batch 8 c + w is warp w of block c: the rows are already
                // batch-major per block, in block order
                detail::check(cudaMemcpy(staging.get(), d_rows,
                                         static_cast<std::size_t>(n_batches) * np * sizeof(double),
                                         cudaMemcpyDeviceToHost),
                              "cudaMemcpy");
                for (uint64_t c = 0; c < count; ++c)
                {
                    const uint64_t g = b0 + c;
                    while (g >= slice_begin(slice + 1))
                        ++slice;
                    FoldSlice &f = slices[static_cast<std::size_t>(slice - c0)];
                    for (int w = 0; w < LB::kWarpsPerBlock; ++w)
                    {
                        const long long nb = warp_units(g % bps, w, req.n_units);
                        if (nb == 0)
                            continue;
                        const double *sum = staging.get() + c * block_doubles + static_cast<std::size_t>(w) * np;
                        if (rqmc)
                        {
                            for (std::size_t p = 0; p < np; ++p)
                                f.s1[p] += sum[p];
                            continue;
                        }
                        const double n = static_cast<double>(nb);
                        if (f.batches == 0)
                            for (std::size_t p = 0; p < np; ++p)
                                f.shift[p] = sum[p] / n;
                        for (std::size_t p = 0; p < np; ++p)
                        {
                            const double y = sum[p] / n - f.shift[p];
                            f.s1[p] += n * y;
                            f.s1n[p] += n * n * y;
                            f.s2[p] += n * n * y * y;
                        }
                        f.total_units += n;
                        f.sum_n2 += n * n;
                        ++f.batches;
                    }
                }
            };
            void *fn = compiled.on(req, device);
            uint64_t n_units = req.n_units, per_segment = bps;
            const detail::LaunchUnits<double> launch = [&](uint64_t b, uint64_t count, double *d_samples)
            {
                // a unit past n_units writes no trail: its length must read 0
                detail::check(cudaMemsetAsync(d_count, 0, urows * sizeof(std::int32_t)), "cudaMemset");
                void *args[] = {&b, &per_segment, &n_units, &count, &path, &d_samples};
                jit::launch(fn, detail::unit_grid(count), LB::kThreadsPerBlock, args);
            };
            const std::vector<WelfordAccumulator> price = detail::run_block_range_units<WelfordAccumulator, double>(
                device, first, last, bps, req.n_units, per_launch, launch, after);
            int overflow = 0;
            detail::check(cudaMemcpy(&overflow, d_overflow, sizeof(int), cudaMemcpyDeviceToHost), "cudaMemcpy");
            if (overflow)
                throw InvalidInput("GPU: a path's gradient trail passed the bound the script's IR gives (a bug: "
                                   "please report it)");
            for (uint64_t c = c0; c < c1; ++c)
                slices[static_cast<std::size_t>(c - c0)].price.assign(
                    price.begin() + static_cast<std::ptrdiff_t>(slice_begin(c) - first),
                    price.begin() + static_cast<std::ptrdiff_t>(slice_begin(c + 1) - first));
            return slices;
        };
        // as many cards as the blocks would use, sharing whole slices
        const std::vector<FoldSlice> slices = detail::run_on_n_devices<FoldSlice>(
            devices_of(req), devices_used(devices_of(req).size(), bps * reps), n_slices, job);

        // The slices merge in their order -- the same sums whatever the
        // number of devices. Pseudo-random: batches of one warp (512 paths),
        // the risks' mean the total over the paths, their error the spread
        // of the batch means (ratio estimator: tail batches may be shorter),
        // each slice's sums re-centred on the first slice's shift (exact:
        // y' = y + d gives sum n y' = sum n y + d sum n, and so on).
        // RQMC: one sum per replicate, the error the replicates' spread.
        std::vector<double> shift(np, 0.0), s1(shift), s2(shift), s1n(shift);
        std::vector<double> rep_sum(static_cast<std::size_t>(reps) * np, 0.0);
        double total_units = 0.0, sum_n2 = 0.0;
        long long batches = 0;
        for (uint64_t c = 0; c < slices.size(); ++c)
        {
            const FoldSlice &f = slices[static_cast<std::size_t>(c)];
            if (rqmc)
            {
                double *r = &rep_sum[static_cast<std::size_t>(c / per_rep) * np];
                for (std::size_t p = 0; p < np; ++p)
                    r[p] += f.s1[p];
                continue;
            }
            if (f.batches == 0)
                continue;
            if (batches == 0)
                shift = f.shift;
            for (std::size_t p = 0; p < np; ++p)
            {
                const double d = f.shift[p] - shift[p];
                s1[p] += f.s1[p] + d * f.total_units;
                s2[p] += f.s2[p] + 2.0 * d * f.s1n[p] + d * d * f.sum_n2;
                s1n[p] += f.s1n[p] + d * f.sum_n2;
            }
            total_units += f.total_units;
            sum_n2 += f.sum_n2;
            batches += f.batches;
        }

        ScriptAdjointGpuResult res;
        res.note = "adjoint kernel, " + compiled.note;
        res.risks.assign(np, 0.0);
        res.risk_std_errors.assign(np, 0.0);
        std::vector<WelfordAccumulator> block_price;
        for (const FoldSlice &f : slices)
            block_price.insert(block_price.end(), f.price.begin(), f.price.end());
        const std::vector<WelfordAccumulator> price = detail::merge_segments(block_price, reps, bps);
        if (rqmc)
        {
            for (const WelfordAccumulator &p : price)
            {
                res.price.add(p.mean); // replicate means: its std_error() is RQMC's
                res.replicate_prices.push_back(p.mean);
            }
            res.replicate_risks.resize(static_cast<std::size_t>(reps) * np);
            for (std::size_t i = 0; i < res.replicate_risks.size(); ++i)
                res.replicate_risks[i] = rep_sum[i] / static_cast<double>(req.n_units);
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
