#ifndef GPU_SCRIPT_UNITS_CUH
#define GPU_SCRIPT_UNITS_CUH

// The units of the GPU script kernel (src/gpu/script.cu), shared with the
// kernel the compiled scripts link at run time (script_jit_kernel.cu, lot P1b
// of blueprint/wp/26-performance.md): both must lay them out the same way.

#include <cstdint>
#include <type_traits>

#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/engines/mc/script_path.hpp"
#include "quantModeling/engines/mc/sobol_bridge.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/variance_reduction/multi_control.hpp"

namespace quantModeling::gpu::detail
{

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

    /// One unit: a path, or the antithetic pair; with controls, the
    /// regression sample (payoff, controls), averaged over the pair.
    /// Importance sampling weights each path by its likelihood ratio.
    template <bool Controlled, class Events = mc::InterpretedEvents>
    struct ScriptUnit
    {
        mc::ScriptPathView view;
        UnitDraws draws;
        bool antithetic;
        uint64_t per_replicate; ///< units per replicate
        Events events;          ///< how an event runs: interpreted, or compiled (lot P1b)

        using Out = std::conditional_t<Controlled, MultiControlAccumulator::Sample, double>;

        /// Unit u of replicate `rep` (its Philox counter: rep * n + u).
        __device__ Out operator()(uint64_t rep, uint64_t u) const
        {
            const mc::ScriptModelInputs<double> in = mc::script_inputs<double>(view);
            const mc::PathDraws d0 = draws.begin(rep, u, rep * per_replicate);
            double x[MultiControlAccumulator::kMaxControls] = {};
            mc::PathDraws d = d0;
            const double a = mc::script_path<double>(view, in, d, 1.0, Controlled ? x : nullptr, events);
            const double wa = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
            if constexpr (!Controlled)
            {
                if (!antithetic)
                    return a * wa;
                d = d0;
                const double b = mc::script_path<double>(view, in, d, -1.0, nullptr, events);
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
                const double b = mc::script_path<double>(view, in, d, -1.0, y, events);
                const double wb = d.n_theta > 0 ? exp(d.log_weight) : 1.0;
                s.v[0] = 0.5 * (s.v[0] + b * wb);
                for (int c = 0; c < view.controls.n; ++c)
                    s.v[1 + c] = 0.5 * (s.v[1 + c] + y[c] * wb);
                return s;
            }
        }
    };

    /**
     * Events run by the script's generated code (lot P1b): the function
     * scripting::cuda_event_source writes, compiled with NVRTC and linked into
     * script_jit_kernel.cu at run time. Only that kernel calls it; the host
     * merely lays the unit out.
     */
    struct CompiledEvents
    {
        __device__ void operator()(int e, scripting::Machine<double> &m, const double *spots, const double *disc,
                                   const double &numeraire) const;
    };

} // namespace quantModeling::gpu::detail

#endif // GPU_SCRIPT_UNITS_CUH
