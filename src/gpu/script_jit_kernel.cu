// The kernels of the compiled scripts (blueprint/wp/26-performance.md §5, lot
// P1b; generated from the IR since blueprint/wp/27-ir-ad.md, lots L2-L4).
//
// A path is blocks of generated code -- the state's start, the model's step,
// the script's events, and for the risks their tangents or adjoints
// (scripting::cuda_path_source) -- and, here, what is the same for every
// script: the loop over the steps, the draws, the antithetic pair, the
// checkpoints of the adjoint, the logical blocks.
//
// Not part of the library's objects: CMake compiles this file three times to
// LTO-IR, once per QM_JIT_KERNEL (0: prices, 1: tangent, 2: adjoint), and
// embeds the fatbins. At run time src/gpu/script_jit.cpp compiles a script's
// blocks with NVRTC, also to LTO-IR, and nvJitLink links the two with
// link-time optimisation, so the blocks are inlined into the path loop and
// the path's state stays in registers.

#include "logical_blocks.cuh"
#include "script_units.cuh"

#include "quantModeling/scripting/ir_grid.hpp"

#ifndef QM_JIT_KERNEL
#error "QM_JIT_KERNEL selects the kernel: 0 prices, 1 tangent, 2 adjoint"
#endif

// The generated blocks and the path's shape.
extern "C" __device__ void qm_init(const QmFrame *f);
extern "C" __device__ void qm_step(const QmFrame *f);
extern "C" __device__ void qm_event(int kind, const QmFrame *f);
extern "C" __device__ int qm_n_state();
extern "C" __device__ int qm_n_model();
extern "C" __device__ int qm_pay_slot();
extern "C" __device__ int qm_factors();
extern "C" __device__ int qm_step_cols();
extern "C" __device__ int qm_event_cols();
extern "C" __device__ int qm_n_risks();
extern "C" __device__ int qm_surv0();
extern "C" __device__ int qm_n_surv();

// The request the kernel is linked for (scripting::CudaKernelSpec), defined
// next to the blocks: constants after link-time optimisation.
extern "C" __device__ int qm_spec_sobol();
extern "C" __device__ int qm_spec_stratified();
extern "C" __device__ int qm_spec_importance();
extern "C" __device__ int qm_spec_antithetic();
extern "C" __device__ int qm_spec_controls();

namespace
{
    using quantModeling::Real;
    using quantModeling::gpu::detail::PathUnit;
    namespace mc = quantModeling::mc;
    namespace grid = quantModeling::scripting::ir::detail;

    __device__ __forceinline__ mc::GridView<Real> grid_of(const QmFrame *f)
    {
        return {f->gK, f->nK, f->gT, f->nT, f->th + f->grid0};
    }

    __device__ __forceinline__ QmFrame frame_of(const PathUnit &u)
    {
        QmFrame f{};
        f.th = u.theta;
        f.tm = u.terms;
        f.gK = u.grid_K;
        f.gT = u.grid_T;
        f.nK = u.grid_nK;
        f.nT = u.grid_nT;
        f.grid0 = u.grid0;
        return f;
    }

    /// The unit with the spec's constants in place of the arguments they
    /// equal: the other samplers' code is dead, and goes.
    __device__ __forceinline__ PathUnit specialised(const PathUnit &fn)
    {
        PathUnit unit = fn;
        unit.antithetic = qm_spec_antithetic() != 0;
        unit.draws.base.given = nullptr;
        if (!qm_spec_sobol())
            unit.draws.sobol.dim = 0;
        if (!qm_spec_stratified())
            unit.draws.base.strata = 0;
        if (!qm_spec_importance())
            unit.draws.base.n_theta = 0;
        unit.controls.n = qm_spec_controls();
        return unit;
    }
} // namespace

// The backward sweep's grid state: the cell of the step being differentiated
// (the local vol's or the SLV leverage's) and the path's sparse gradient
// trail.
//
// The trail (issue #342): the path's grid adjoints as (parameter, adjoint)
// pairs in the order the sweep writes them, not a dense row over every
// parameter. A step reaches two leverage nodes (SLV) or eight local-vol nodes
// out of the page's 12 900, so the dense row -- 100 KB a thread -- was over
// 96 % zeros, held the adjoint at 8 paths a thread and 32 blocks on the card
// (12.5 % occupancy, latency-bound), and its memset and fold cost more than
// the kernel. The fold (src/gpu/script.cu, fold_trails) sums the pairs into
// one row per batch in a fixed order.
struct GridTrail
{
    std::int32_t *index = nullptr; ///< pair e at index[e * stride]
    double *value = nullptr;
    long stride = 0;
    int n = 0;   ///< pairs written (may pass cap: the fold reports it)
    int cap = 0; ///< pairs the arrays hold

    __device__ __forceinline__ void add(long p, double a)
    {
        if (n < cap)
        {
            index[n * stride] = static_cast<std::int32_t>(p);
            value[n * stride] = a;
        }
        ++n;
    }
};

struct BackwardCell
{
    union
    {
        grid::GridCell vol;
        grid::LeverageCell leverage;
    } cell;
    GridTrail trail;
};

// The grid's lookups: the models' own functions (path_steps.hpp), so that a
// generated local-vol step is local_vol_step's, bit for bit.
//
// The adjoint's backward sweep (f->cell set) asks three things of the same
// cell -- the vol, its slope, the corners' shares: the cell is looked up once,
// by the vol, and kept for the other two (the adjoint written by hand did
// the same). The vol is then the cell's own bilinear value, the same
// expression as local_vol_bilinear's.
extern "C" __device__ double qm_grid_vol(const QmFrame *f, double spot, double t)
{
    if (f->cell)
    {
        grid::GridCell &c = static_cast<BackwardCell *>(f->cell)->cell.vol;
        c = grid::grid_cell(grid_of(f), spot, t);
        return c.sigma < 1e-6 ? 1e-6 : c.sigma;
    }
    return mc::local_vol_bilinear<Real>(grid_of(f), spot, t);
}
extern "C" __device__ double qm_grid_slope(const QmFrame *f, double spot, double t)
{
    if (f->cell)
        return grid::grid_slope(grid_of(f), static_cast<const BackwardCell *>(f->cell)->cell.vol);
    return grid::grid_slope(grid_of(f), spot, t);
}
// The accumulations run in the adjoint's backward sweep alone (f->cell set).
// The local vol's eight nodes go straight to the trail, as the leverage's
// two: every lane writes the same number of pairs a step, so pair e of the
// warp's 32 paths is one coalesced store. The per-thread buffer that summed
// a cell's nodes over consecutive steps (2026-11-16, against the dense row's
// read-modify-writes) desynchronised the lanes -- each flushed when its own
// cell changed, every store a sector of its own -- and took 75 registers:
// the phoenix autocall on the page's local-vol grid ran its adjoint in 47 ms
// with it, 7.1 ms without (65 536 paths, one V100, 2026-11-20).
extern "C" __device__ void qm_grid_accumulate(const QmFrame *f, double spot, double t, double adjoint)
{
    BackwardCell &b = *static_cast<BackwardCell *>(f->cell);
    grid::grid_accumulate_to(b.cell.vol, f->nT, adjoint, f->grid0, [&](long p, double v)
                             { b.trail.add(p, v); });
}
// The SLV leverage the same way: on the backward sweep its cell is looked up
// once, by the leverage, and kept for its slope and its corners' shares
// (WP 27 §14.6 left SLV with three lookups a step). The leverage is then the
// cell's own expression, mc::leverage_staircase's.
extern "C" __device__ double qm_grid_leverage(const QmFrame *f, double spot, double t)
{
    if (f->cell)
    {
        grid::LeverageCell &c = static_cast<BackwardCell *>(f->cell)->cell.leverage;
        c = grid::leverage_cell(grid_of(f), spot, t);
        return grid::leverage_value(grid_of(f), c);
    }
    return mc::leverage_staircase<Real>(grid_of(f), spot, t);
}
extern "C" __device__ double qm_grid_leverage_slope(const QmFrame *f, double spot, double t)
{
    if (f->cell)
        return grid::leverage_slope(grid_of(f), static_cast<const BackwardCell *>(f->cell)->cell.leverage);
    return grid::leverage_slope(grid_of(f), spot, t);
}
extern "C" __device__ void qm_grid_leverage_accumulate(const QmFrame *f, double spot, double t, double adjoint)
{
    BackwardCell &b = *static_cast<BackwardCell *>(f->cell);
    grid::leverage_accumulate_to(b.cell.leverage, f->nT, adjoint, f->grid0,
                                 [&](long p, double v)
                                 { b.trail.add(p, v); });
}
// A grid parameter's adjoint the generated code writes as a block's output.
extern "C" __device__ void qm_theta_add(const QmFrame *f, int index, double adjoint)
{
    static_cast<BackwardCell *>(f->cell)->trail.add(index, adjoint);
}

// The step's gaussians, `draw(i, k)` the k-th of drawing step i. On
// multilevel's coarse path conditioned on the fine one (qm_half_col() >= 0,
// WP 26 §26.2) the request carries the fine level's draws: coarse step d
// takes fine steps 2d and 2d + 1, z = sqrt(w) z1 + sqrt(1 - w) z2 with w the
// first's share of the step (its data row), and z[factors] = sqrt(w) z1, the
// part of the draw the IR's conditioned step reads -- the coarse increments
// summed from the fine ones, as mlmc_detail::LevelSampler builds them on the
// CPU. Otherwise one draw a factor, as always; the generated constant drops
// the other branch.
extern "C" __device__ int qm_half_col();

namespace
{
    template <class Draw>
    __device__ __forceinline__ void step_draws(const Draw &draw, int d, const double *dat, double *z, int factors)
    {
        const int hc = qm_half_col();
        if (hc < 0)
        {
            for (int k = 0; k < factors; ++k)
                z[k] = draw(d, k);
            return;
        }
        const double w = dat[hc], wa = sqrt(w), wb = sqrt(1.0 - w);
        for (int k = 0; k < factors; ++k)
        {
            const double z1 = draw(2 * d, k);
            z[k] = wa * z1 + wb * draw(2 * d + 1, k);
            if (k == 0)
                z[factors] = wa * z1;
        }
    }
} // namespace

#if QM_JIT_KERNEL == 0
// ── prices ──────────────────────────────────────────────────────────────────

extern "C" __device__ double qm_path(const void *unit, void *draws, double sign, double *controls);

/// One path: mc::script_path's loop, on the generated blocks. `x`, when not
/// null, receives the spot controls.
extern "C" __device__ double qm_drive(const void *unit_, void *draws_, double sign, double *x, double *st)
{
    const PathUnit &u = *static_cast<const PathUnit *>(unit_);
    mc::PathDraws &draws = *static_cast<mc::PathDraws *>(draws_);
    double z[mc::ScriptLimits::kAssets + 1]; // + 1: the conditioned coarse path's part a (step_draws)
    QmFrame f = frame_of(u);
    f.st = st;
    f.z = z;
    f.dat = u.baseline;
    qm_init(&f);
    const int factors = qm_factors();
    const long step_cols = qm_step_cols(), event_cols = qm_event_cols();
    int d = 0; // drawing steps so far
    for (int s = 0; s < u.n_steps; ++s)
    {
        if (u.step_draws[s])
        {
            f.dat = u.step_rows + s * step_cols;
            step_draws([&](int i, int k) { return draws.draw(i, k, sign); }, d, f.dat, z, factors);
            qm_step(&f);
            ++d;
        }
        const int e = u.step_event[s];
        if (e < 0)
            continue;
        f.dat = u.event_rows + e * event_cols;
        f.tm = u.terms + u.term_begin[e];
        qm_event(u.kind[e], &f);
        if (x)
            for (int c = 0; c < u.controls.n; ++c)
                if (u.controls.event[c] == e && u.controls.asset[c] >= 0)
                    x[c] = st[u.controls.asset[c]] / f.dat[0]; // the spot, deflated by the date's numeraire
    }
    // A script's analytic control (controlPays): asset -(i + 1) reads
    // variable i at the end of the path, already deflated.
    if (x)
        for (int c = 0; c < u.controls.n; ++c)
            if (u.controls.asset[c] < 0)
                x[c] = st[qm_n_model() - (u.controls.asset[c] + 1)];
    return st[qm_pay_slot()];
}

namespace
{
    /// gpu::detail::ScriptUnit, on the generated path: a path or the
    /// antithetic pair; with controls, the regression sample.
    template <bool Controlled>
    struct CompiledUnit
    {
        PathUnit unit;
        using Out = std::conditional_t<Controlled, quantModeling::MultiControlAccumulator::Sample, double>;

        __device__ Out operator()(uint64_t rep, uint64_t u) const
        {
            using quantModeling::MultiControlAccumulator;
            const mc::PathDraws d0 = unit.draws.begin(rep, u, rep * unit.per_replicate);
            double x[MultiControlAccumulator::kMaxControls] = {};
            mc::PathDraws d = d0;
            const double a = qm_path(&unit, &d, 1.0, Controlled ? x : nullptr);
            const double wa = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
            if constexpr (!Controlled)
            {
                if (!unit.antithetic)
                    return a * wa;
                d = d0;
                const double b = qm_path(&unit, &d, -1.0, nullptr);
                const double wb = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                return 0.5 * (a * wa + b * wb);
            }
            else
            {
                Out s;
                s.v[0] = a * wa;
                for (int c = 0; c < unit.controls.n; ++c)
                    s.v[1 + c] = x[c] * wa;
                if (!unit.antithetic)
                    return s;
                double y[MultiControlAccumulator::kMaxControls] = {};
                d = d0;
                const double b = qm_path(&unit, &d, -1.0, y);
                const double wb = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                s.v[0] = 0.5 * (s.v[0] + b * wb);
                for (int c = 0; c < unit.controls.n; ++c)
                    s.v[1 + c] = 0.5 * (s.v[1 + c] + y[c] * wb);
                return s;
            }
        }
    };
} // namespace

/// Prices without controls: Philox, stratified or Sobol, antithetic or not,
/// importance sampling. One thread per unit (lot P1g): each stores its
/// payoff in `samples`, which the library's logical_reduce_kernel reduces in
/// logical-block order.
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock)
    qm_script_plain(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                    quantModeling::gpu::detail::PathUnit fn, double *samples)
{
    const CompiledUnit<false> unit{specialised(fn)};
    quantModeling::gpu::detail::logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, unit,
                                                    samples);
}

/// The same at four blocks a multiprocessor (64 registers a thread), for
/// Heston and SLV: their price kernels used 96-106 registers, two blocks a
/// multiprocessor, and a page launch of 256 blocks ran as 1.6 waves. Capped,
/// it is one wave: -12 to -14 % under SLV, -9 % under Heston (one V100,
/// 50 000 Sobol paths, the library's products, 2026-11-19, lot B of #331),
/// the same bits. Not for local vol, whose 127 registers spill: +44 %.
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock, 4)
    qm_script_plain_r64(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                        quantModeling::gpu::detail::PathUnit fn, double *samples)
{
    const CompiledUnit<false> unit{specialised(fn)};
    quantModeling::gpu::detail::logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, unit,
                                                    samples);
}

/// With spot control variates: each unit's regression sample.
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock)
    qm_script_controlled(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                         quantModeling::gpu::detail::PathUnit fn,
                         quantModeling::MultiControlAccumulator::Sample *samples)
{
    const CompiledUnit<true> unit{specialised(fn)};
    quantModeling::gpu::detail::logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, unit,
                                                    samples);
}

#elif QM_JIT_KERNEL == 1
// ── tangent: the price and its derivative against every scalar parameter ────

extern "C" __device__ void qm_path(const void *unit, void *draws, double *out);

/// One path, the state and its tangents advanced together by the generated
/// blocks: out[0] the payoff, out[1 + d] its derivative in direction d.
extern "C" __device__ void qm_drive(const void *unit_, void *draws_, double *out, double *st, double *dst)
{
    const PathUnit &u = *static_cast<const PathUnit *>(unit_);
    mc::PathDraws &draws = *static_cast<mc::PathDraws *>(draws_);
    const int K = qm_n_risks(), n_state = qm_n_state();
    for (int i = 0; i < n_state * K; ++i)
        dst[i] = 0.0;
    double z[mc::ScriptLimits::kAssets + 1]; // + 1: the conditioned coarse path's part a (step_draws)
    QmFrame f = frame_of(u);
    f.st = st;
    f.dst = dst;
    f.z = z;
    f.dat = u.baseline;
    qm_init(&f);
    const int factors = qm_factors();
    const long step_cols = qm_step_cols(), event_cols = qm_event_cols();
    int d = 0;
    for (int s = 0; s < u.n_steps; ++s)
    {
        if (u.step_draws[s])
        {
            f.dat = u.step_rows + s * step_cols;
            step_draws([&](int i, int k) { return draws.draw(i, k, 1.0); }, d, f.dat, z, factors);
            qm_step(&f);
            ++d;
        }
        const int e = u.step_event[s];
        if (e < 0)
            continue;
        f.dat = u.event_rows + e * event_cols;
        f.tm = u.terms + u.term_begin[e];
        qm_event(u.kind[e], &f);
    }
    const int pay = qm_pay_slot();
    out[0] = st[pay];
    for (int k = 0; k < K; ++k)
        out[1 + k] = dst[pay * K + k];
}

extern "C" __device__ int qm_last0();

/// The final date at X = m + s z: the state and tangents the last step left
/// (st0, dst0), the spot set to exp(X) with its tangent held at 0 (X fixed),
/// then the dates from the last step to the final one. The payoff, and its
/// tangents in d (fixed X).
__device__ double qm_vibrato_at(const PathUnit &u, QmFrame &f, const double *st0, const double *dst0, int s_last,
                                int s_final, double X, double *d)
{
    const int K = qm_n_risks(), n_state = qm_n_state();
    for (int i = 0; i < n_state; ++i)
        f.st[i] = st0[i];
    for (int i = 0; i < n_state * K; ++i)
        f.dst[i] = dst0[i];
    f.st[0] = exp(X);
    for (int k = 0; k < K; ++k)
        f.dst[k] = 0.0;
    const long event_cols = qm_event_cols();
    for (int s = s_last; s <= s_final; ++s)
    {
        const int e = u.step_event[s];
        if (e < 0)
            continue;
        f.dat = u.event_rows + e * event_cols;
        f.tm = u.terms + u.term_begin[e];
        qm_event(u.kind[e], &f);
    }
    const int pay = qm_pay_slot();
    for (int k = 0; k < K; ++k)
        d[k] = f.dst[pay * K + k];
    return f.st[pay];
}

/// Vibrato (blueprint/wp/27-ir-ad.md §14.11; the host's ir::run_path_vibrato):
/// the path and its tangents up to the last step into the final date, then a
/// likelihood ratio over that step, lognormal with the forward F and variance
/// v it leaves at qm_last0(). out[0] the payoff, out[1 + d] its derivative.
extern "C" __device__ void qm_drive_vibrato(const void *unit_, void *draws_, double *out, double *st, double *dst,
                                            double *st0, double *dst0)
{
    const PathUnit &u = *static_cast<const PathUnit *>(unit_);
    mc::PathDraws &draws = *static_cast<mc::PathDraws *>(draws_);
    const int K = qm_n_risks(), n_state = qm_n_state();
    constexpr int kMaxK = 8; // one underlying: Black-Scholes's four risks, local vol's three
    for (int i = 0; i < n_state * K; ++i)
        dst[i] = 0.0;
    int s_final = -1, s_last = -1;
    for (int s = 0; s < u.n_steps; ++s)
        if (u.step_event[s] >= 0)
            s_final = s;
    for (int s = 0; s <= s_final; ++s)
        if (u.step_draws[s])
            s_last = s;
    double z[mc::ScriptLimits::kAssets + 1]; // + 1: the conditioned coarse path's part a (step_draws)
    QmFrame f = frame_of(u);
    f.st = st;
    f.dst = dst;
    f.z = z;
    f.dat = u.baseline;
    qm_init(&f);
    const int factors = qm_factors();
    const long step_cols = qm_step_cols(), event_cols = qm_event_cols();
    int d = 0;
    for (int s = 0; s < s_last; ++s)
    {
        if (u.step_draws[s])
        {
            f.dat = u.step_rows + s * step_cols;
            step_draws([&](int i, int k) { return draws.draw(i, k, 1.0); }, d, f.dat, z, factors);
            qm_step(&f);
            ++d;
        }
        const int e = u.step_event[s];
        if (e < 0)
            continue;
        f.dat = u.event_rows + e * event_cols;
        f.tm = u.terms + u.term_begin[e];
        qm_event(u.kind[e], &f);
    }
    for (int k = 0; k < factors; ++k)
        z[k] = draws.draw(d, k, 1.0);
    f.dat = u.step_rows + s_last * step_cols;
    qm_step(&f);
    for (int i = 0; i < n_state; ++i)
        st0[i] = st[i];
    for (int i = 0; i < n_state * K; ++i)
        dst0[i] = dst[i];
    const int L = qm_last0();
    const double F = st0[L], v = st0[L + 1];
    const double sd = sqrt(v), m = log(F) - 0.5 * v, Z = z[0];
    double up[kMaxK], down[kMaxK], none[kMaxK];
    const double b_up = qm_vibrato_at(u, f, st0, dst0, s_last, s_final, m + sd * Z, up);
    const double b_down = qm_vibrato_at(u, f, st0, dst0, s_last, s_final, m - sd * Z, down);
    const double b0 = qm_vibrato_at(u, f, st0, dst0, s_last, s_final, m, none);
    const double g_m = 0.5 * (b_up - b_down) * Z / sd;
    const double g_s = (0.5 * (b_up + b_down) - b0) * (Z * Z - 1.0) / sd;
    out[0] = 0.5 * (b_up + b_down);
    for (int k = 0; k < K && k < kMaxK; ++k)
    {
        const double dF = dst0[L * K + k], dv = dst0[(L + 1) * K + k];
        out[1 + k] = 0.5 * (up[k] + down[k]) + g_m * (dF / F - 0.5 * dv) + g_s * dv / (2.0 * sd);
    }
}

/// One thread per unit (a path, no mirror): its payoff and derivatives to
/// samples[i * stride ...], a Dual<stride - 1> the library's
/// logical_reduce_kernel reduces in logical-block order.
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock)
    qm_script_tangent(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                      quantModeling::gpu::detail::PathUnit fn, double *samples, int stride)
{
    using L = quantModeling::mc::LogicalBlocks;
    const PathUnit unit = specialised(fn);
    const uint64_t i = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n_blocks * L::kUnitsPerBlock)
        return;
    const uint64_t g = first_block + i / L::kUnitsPerBlock;
    const uint64_t u = (g % blocks_per_segment) * L::kUnitsPerBlock + i % L::kUnitsPerBlock;
    if (u >= n_units)
        return;
    const uint64_t rep = g / blocks_per_segment;
    double *out = samples + i * static_cast<uint64_t>(stride);
    for (int k = 0; k < stride; ++k)
        out[k] = 0.0;
    mc::PathDraws d = unit.draws.begin(rep, u, rep * unit.per_replicate);
    qm_path(&unit, &d, out);
}

#else
// ── adjoint: the price and its gradient against every parameter ─────────────

extern "C" __device__ void qm_init_adjoint(const QmFrame *f);
extern "C" __device__ void qm_step_adjoint(const QmFrame *f);
extern "C" __device__ void qm_event_adjoint(int kind, const QmFrame *f);
extern "C" __device__ double qm_path(const void *unit, void *draws);

/**
 * One path forward, then back (blueprint/wp/27-ir-ad.md §5.2). Forward, the
 * thread checkpoints what each block is about to overwrite: the model's
 * state (and the bridge survivals' running products, which a step reads and
 * multiplies) before a step, the script's variables and the payoff before a
 * date.
 * Backward, it puts them back -- `st` is rolled back in place, last in first
 * out -- and runs the block's adjoint on the state the block started from.
 * The draws are not stored: Philox and the Sobol point regenerate them.
 * The grid's adjoints go to the path's sparse trail as the steps reach its
 * cells; the scalar parameters' stay in `lth` for the path and close the
 * trail, parameters 0 to n_risks - 1.
 */
extern "C" __device__ double qm_drive(const void *unit_, void *draws_, double *st, double *ast, double *lth)
{
    const PathUnit &u = *static_cast<const PathUnit *>(unit_);
    const mc::PathDraws &draws = *static_cast<const mc::PathDraws *>(draws_);
    const long row = static_cast<long>(blockIdx.x) * blockDim.x + threadIdx.x;
    double *ck = u.checkpoints + row;
    const long rows = u.rows;
    const int n_state = qm_n_state(), n_model = qm_n_model(), factors = qm_factors(), n_risks = qm_n_risks();
    const int surv0 = qm_surv0(), n_surv = qm_n_surv();
    const long step_cols = qm_step_cols(), event_cols = qm_event_cols();
    double z[mc::ScriptLimits::kAssets + 1]; // + 1: the conditioned coarse path's part a (step_draws)
    QmFrame f = frame_of(u);
    f.st = st;
    f.z = z;
    f.dat = u.baseline;
    qm_init(&f);
    long c = 0;
    int d = 0;
    for (int s = 0; s < u.n_steps; ++s)
    {
        if (u.step_draws[s])
        {
            for (int k = 0; k < n_model; ++k)
                ck[(c++) * rows] = st[k];
            for (int j = 0; j < n_surv; ++j)
                ck[(c++) * rows] = st[surv0 + j];
            f.dat = u.step_rows + s * step_cols;
            step_draws([&](int i, int k) { return draws.at(i, k); }, d, f.dat, z, factors);
            qm_step(&f);
            ++d;
        }
        const int e = u.step_event[s];
        if (e < 0)
            continue;
        for (int i = n_model; i < n_state; ++i)
            ck[(c++) * rows] = st[i];
        f.dat = u.event_rows + e * event_cols;
        f.tm = u.terms + u.term_begin[e];
        qm_event(u.kind[e], &f);
    }
    const double payoff = st[qm_pay_slot()];

    for (int i = 0; i < n_state; ++i)
        ast[i] = 0.0;
    ast[qm_pay_slot()] = 1.0;
    for (int p = 0; p < n_risks; ++p)
        lth[p] = 0.0;
    // the cell of a backward step, and the path's trail
    BackwardCell cell{};
    cell.trail.index = u.trail_index + row;
    cell.trail.value = u.trail_value + row;
    cell.trail.stride = rows;
    cell.trail.cap = u.trail_cap;
    f.ast = ast;
    f.lth = lth;
    f.cell = &cell;
    for (int s = u.n_steps - 1; s >= 0; --s)
    {
        const int e = u.step_event[s];
        if (e >= 0)
        {
            for (int i = n_state - 1; i >= n_model; --i)
                st[i] = ck[(--c) * rows];
            f.dat = u.event_rows + e * event_cols;
            f.tm = u.terms + u.term_begin[e];
            qm_event_adjoint(u.kind[e], &f);
        }
        if (u.step_draws[s])
        {
            --d;
            for (int j = n_surv - 1; j >= 0; --j)
                st[surv0 + j] = ck[(--c) * rows];
            for (int k = n_model - 1; k >= 0; --k)
                st[k] = ck[(--c) * rows];
            f.dat = u.step_rows + s * step_cols;
            step_draws([&](int i, int k) { return draws.at(i, k); }, d, f.dat, z, factors);
            qm_step_adjoint(&f);
        }
    }
    f.dat = u.baseline;
    qm_init_adjoint(&f);
    for (int p = 0; p < n_risks; ++p)
        cell.trail.add(p, lth[p]);
    u.trail_count[row] = cell.trail.n;
    return payoff;
}

namespace
{
    struct AdjointUnit
    {
        PathUnit unit;
        using Out = double;

        __device__ double operator()(uint64_t rep, uint64_t u) const
        {
            mc::PathDraws d = unit.draws.begin(rep, u, rep * unit.per_replicate);
            return qm_path(&unit, &d);
        }
    };
} // namespace

/// One thread per unit (a path), as the price kernels (lot P1g): thread i
/// runs unit i % 2 048 of the launch's logical block i / 2 048 and stores its
/// payoff at samples[i]; the host's logical_reduce_kernel reduces them in the
/// serial kernel's order. Its gradient is its trail, which fold_trails sums.
/// With a dense row per thread the adjoint ran 8 units a thread on 32 blocks:
/// the phoenix autocall under SLV on the page's 100 x 129 grid (65 536 paths)
/// went from 56.5 ms to 17.3 ms (+ 5.3 ms of fold), the whole call from 122 to
/// 32 ms (one V100, 2026-11-20, issue #342; WP 26 §22).
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock)
    qm_script_adjoint(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                      quantModeling::gpu::detail::PathUnit fn, double *samples)
{
    PathUnit unit = specialised(fn);
    unit.antithetic = false;
    const AdjointUnit paths{unit};
    quantModeling::gpu::detail::logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, paths,
                                                    samples);
}

#endif
