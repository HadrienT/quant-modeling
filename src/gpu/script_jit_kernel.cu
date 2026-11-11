// The script kernel of lot P1b (blueprint/wp/26-performance.md §5): the GPU
// script kernel of src/gpu/script.cu with the events run by a script's
// generated code instead of the bytecode interpreter.
//
// Not part of the library's objects: CMake compiles it to LTO-IR
// (-gencode code=lto_XX -rdc=true -fatbin) and embeds the fatbin. At run time
// src/gpu/script_jit.cpp compiles a script's events with NVRTC, also to
// LTO-IR, and nvJitLink links the two with link-time optimisation, so the
// events are inlined into the path loop and the script's values stay in
// registers.

#include "logical_blocks.cuh"
#include "script_units.cuh"

extern "C" __device__ void qm_script_event(int e, double *vars, const double *spots, const double *discounts,
                                           double numeraire, double *payoff);

// The request the kernel is linked for (scripting::CudaKernelSpec), defined
// next to the events: constants after link-time optimisation.
extern "C" __device__ int qm_spec_kind();
extern "C" __device__ int qm_spec_n_assets();
extern "C" __device__ int qm_spec_sobol();
extern "C" __device__ int qm_spec_stratified();
extern "C" __device__ int qm_spec_importance();
extern "C" __device__ int qm_spec_antithetic();

namespace quantModeling::gpu::detail
{
    __device__ void CompiledEvents::operator()(int e, scripting::Machine<double> &m, const double *spots,
                                               const double *disc, const double &numeraire) const
    {
        qm_script_event(e, m.vars, spots, disc, numeraire, &m.payoff);
    }
} // namespace quantModeling::gpu::detail

/// Prices without controls: Philox, stratified or Sobol, antithetic or not,
/// importance sampling -- simulate_script's `plain` statistics. One thread
/// per unit (lot P1g): each stores its payoff in `samples`, which the
/// library's logical_reduce_kernel reduces in logical-block order.
extern "C" __global__ void __launch_bounds__(quantModeling::mc::LogicalBlocks::kThreadsPerBlock)
    qm_script_plain(uint64_t first_block, uint64_t blocks_per_segment, uint64_t n_units, uint64_t n_blocks,
                    quantModeling::gpu::detail::ScriptUnit<false, quantModeling::gpu::detail::CompiledEvents> fn,
                    double *samples)
{
    using namespace quantModeling;
    // The unit with the spec's constants in place of the arguments they
    // equal: the other models' and samplers' code is dead, and goes.
    gpu::detail::ScriptUnit<false, gpu::detail::CompiledEvents> unit = fn;
    unit.view.kind = static_cast<DeviceModel::Kind>(qm_spec_kind());
    unit.view.n_assets = qm_spec_n_assets();
    unit.antithetic = qm_spec_antithetic() != 0;
    unit.draws.base.given = nullptr;
    if (!qm_spec_sobol())
        unit.draws.sobol.dim = 0;
    if (!qm_spec_stratified())
        unit.draws.base.strata = 0;
    if (!qm_spec_importance())
        unit.draws.base.n_theta = 0;
    gpu::detail::logical_block_units(first_block, blocks_per_segment, n_units, n_blocks, unit, samples);
}
