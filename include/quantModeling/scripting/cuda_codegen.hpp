#ifndef QM_SCRIPTING_CUDA_CODEGEN_HPP
#define QM_SCRIPTING_CUDA_CODEGEN_HPP

#include <string>

#include "quantModeling/scripting/bytecode.hpp"

/**
 * @file cuda_codegen.hpp
 * @brief A compiled script's bytecode turned into CUDA source
 *        (blueprint/wp/26-performance.md §5, lot P1b).
 *
 * The GPU kernel interprets a script's bytecode: one `switch` per
 * instruction, the value stack in local memory. This generator writes the
 * same instructions out as straight-line CUDA, one block per event, the
 * stack slots as named locals and the jumps as `goto`, so that the device
 * compiler sees the script and keeps its values in registers. The GPU
 * backend compiles the source at run time (NVRTC) and links it into the
 * script kernel (src/gpu/script_jit.*).
 *
 * Same bits as the interpreter on the GPU, by construction:
 *  - the same operations in the same order, per path;
 *  - no FMA contraction on either side: the interpreter (script.cu) and the
 *    compiled kernel are built with --fmad=false, and the stack arithmetic
 *    goes through __dadd_rn, __dsub_rn and __dmul_rn besides. With
 *    contraction the bits would depend on how the optimiser sees the code
 *    (measured: Heston moved in the last bit once the kernel was
 *    specialised);
 *  - the fuzzy-if helpers and blends written as the interpreter writes them
 *    (bytecode.hpp);
 *  - constants as hexadecimal floating literals, exact to the bit.
 *
 * The source holds no user text: only indices, opcodes turned into C and
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

    /// The CUDA source of `p`'s events, or why it cannot be generated (a
    /// malformed program: unbalanced stack, a jump out of its event). Events
    /// with the same statements (a schedule's dates) share their code.
    CudaSource cuda_event_source(const Program &p);

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
