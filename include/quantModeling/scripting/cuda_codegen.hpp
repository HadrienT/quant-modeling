#ifndef QM_SCRIPTING_CUDA_CODEGEN_HPP
#define QM_SCRIPTING_CUDA_CODEGEN_HPP

#include <string>

#include "quantModeling/scripting/ir.hpp"

/**
 * @file cuda_codegen.hpp
 * @brief A script's IR turned into CUDA source
 *        (blueprint/wp/27-ir-ad.md §4, lot L1; the run-time compilation is
 *        blueprint/wp/26-performance.md §5, lot P1b).
 *
 * The generator writes each distinct block of the IR (scripting/ir.hpp) as
 * straight-line CUDA: every SSA value a local double, the regions as `if`s.
 * The GPU backend compiles the source at run time (NVRTC) and links it into
 * the script kernel (src/gpu/script_jit.*), so the device compiler sees the
 * script and keeps its values in registers.
 *
 * Same bits as the bytecode interpreter on the GPU, by construction:
 *  - the IR holds the interpreter's operations in its order, per path, and
 *    the generator writes them one for one;
 *  - no FMA contraction on either side: the interpreter (script.cu) and the
 *    compiled kernel are built with --fmad=false, and + - * go through
 *    __dadd_rn, __dsub_rn and __dmul_rn besides. With contraction the bits
 *    would depend on how the optimiser sees the code (measured: Heston moved
 *    in the last bit once the kernel was specialised);
 *  - the fuzzy ramps and blends written as the interpreter writes them;
 *  - constants as hexadecimal floating literals, exact to the bit.
 *
 * The source holds no user text: only indices, operations turned into C and
 * numbers. It needs no header (NVRTC has no standard library).
 */

namespace quantModeling::scripting
{

    /// The device function the generated source defines and the script
    /// kernel calls at each event date:
    ///   extern "C" __device__ void qm_script_event(int e, double *vars,
    ///       const double *spots, const double *discounts, double numeraire,
    ///       double *payoff);
    inline constexpr const char *kCudaEventFunction = "qm_script_event";

    struct CudaSource
    {
        std::string source; ///< empty when the program cannot be generated
        std::string why;    ///< then: the reason, in words a user can read
    };

    /// The CUDA source of `s`'s events, or why it cannot be generated.
    /// Events that share a block (a schedule's dates) share their code.
    CudaSource cuda_event_source(const ir::Script &s);

    /**
     * What one compiled kernel serves: the model and the sampling options of
     * a request. The kernel (src/gpu/script_jit_kernel.cu) reads them as
     * constants instead of its arguments, so that link-time optimisation
     * drops the code of the other models and samplers -- the same operations
     * run on every path, in less code (the instruction cache was the
     * bottleneck: WP 26 §5, P1b). The host builds it from the request it
     * launches, so the constants equal the arguments they replace.
     */
    struct CudaKernelSpec
    {
        int model_kind = 0; ///< DeviceModel::Kind
        int n_assets = 1;
        bool sobol = false;
        bool stratified = false;
        bool importance = false; ///< an importance-sampling drift
        bool antithetic = true;
    };

    /// The spec's constants as CUDA source (qm_spec_* functions).
    std::string cuda_spec_source(const CudaKernelSpec &spec);

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_CUDA_CODEGEN_HPP
