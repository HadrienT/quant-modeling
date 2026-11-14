#ifndef QM_SCRIPTING_CUDA_CODEGEN_HPP
#define QM_SCRIPTING_CUDA_CODEGEN_HPP

#include <string>

#include "quantModeling/scripting/ir.hpp"
#include "quantModeling/scripting/ir_path.hpp"

/**
 * @file cuda_codegen.hpp
 * @brief A path's IR turned into CUDA source
 *        (blueprint/wp/27-ir-ad.md §4 and §6, lots L1-L4; the run-time
 *        compilation is blueprint/wp/26-performance.md §5, lot P1b).
 *
 * The generator writes the blocks of a path (scripting/ir_path.hpp: the
 * state's start, the model's step, each distinct block of the script) as
 * straight-line CUDA functions: every SSA value a local double, the regions
 * as `if`s. For the risks it writes the blocks' tangents or adjoints
 * (scripting/ir_ad.hpp) the same way. The GPU backend compiles the source
 * at run time (NVRTC) and links it into the kernel that loops over the
 * steps (src/gpu/script_jit.*), so the device compiler sees the model and
 * the script together and keeps the path's state in registers.
 *
 * Same bits as the hand-written path on the GPU, by construction:
 *  - the IR holds the interpreter's and the models' operations in their
 *    order, per path, and the generator writes them one for one;
 *  - no FMA contraction on either side: the interpreter (script.cu) and the
 *    compiled kernels are built with --fmad=false, and sums, differences and
 *    products go through __dadd_rn, __dsub_rn and __dmul_rn besides. With
 *    contraction the bits would depend on how the optimiser sees the code
 *    (measured: Heston moved in the last bit once the kernel was
 *    specialised);
 *  - the fuzzy ramps and blends written as the interpreter writes them;
 *  - constants as hexadecimal floating literals, exact to the bit.
 *
 * The source holds no user text: only indices, operations turned into C and
 * numbers. It needs no header (NVRTC has no standard library).
 */

namespace quantModeling::scripting
{

    struct CudaSource
    {
        std::string source; ///< empty when the program cannot be generated
        std::string why;    ///< then: the reason, in words a user can read
    };

    /// What the generated path computes: the price, the price and its
    /// derivative against every scalar parameter of the model (tangent), or
    /// the price and its gradient against every parameter, the local-vol
    /// grid's points included (adjoint).
    enum class PathKernel : int
    {
        Price = 0,
        Tangent = 1,
        Adjoint = 2
    };

    /**
     * @brief The CUDA source of a script on a model kind (DeviceModel::Kind)
     *        with `n_assets` underlyings, or why it cannot be generated.
     *
     * It defines, for the kernel that loops over the steps
     * (src/gpu/script_jit_kernel.cu):
     *
     *     void qm_init(const QmFrame *)             the state at t = 0
     *     void qm_step(const QmFrame *)             one model step
     *     void qm_event(int kind, const QmFrame *)  a date's block
     *     ... qm_init_adjoint, qm_step_adjoint, qm_event_adjoint  (Adjoint)
     *     qm_path(...)   the per-path entry: the state's arrays, sized for
     *                    this script, then the kernel's loop (qm_drive)
     *
     * One `case` per distinct block of the script: which block a date runs,
     * the dates themselves and (ScriptSettings::terms_as_parameters) the
     * product's terms are data. The Tangent kernel's blocks advance the
     * state and its tangents together.
     */
    CudaSource cuda_path_source(const ir::Script &script, int model_kind, int n_assets, PathKernel kernel);

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
        int controls = 0; ///< spot control variates (0: none)
    };

    /// The spec's constants as CUDA source (qm_spec_* functions).
    std::string cuda_spec_source(const CudaKernelSpec &spec);

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_CUDA_CODEGEN_HPP
