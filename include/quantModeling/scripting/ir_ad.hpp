#ifndef QM_SCRIPTING_IR_AD_HPP
#define QM_SCRIPTING_IR_AD_HPP

#include <vector>

#include "quantModeling/scripting/ir.hpp"
#include "quantModeling/scripting/ir_path.hpp"

/**
 * @file ir_ad.hpp
 * @brief Derivatives by source transformation: the tangent and the adjoint
 *        of a block are blocks (blueprint/wp/27-ir-ad.md §5, lots L3-L4).
 *
 * Both transformations take a path-level block (ir_path.hpp: its inputs are
 * State, Draw, Data, Theta and Param) and return a new one. They carry no
 * rule for a model or for a product: only for the IR's operations, each
 * with the local derivatives of the CPU tape and the choices it makes at a
 * kink (aad/expression.hpp; utils/dual.hpp has the same table) -- max(l, r)
 * differentiates l when l > r, min(l, r) when l < r, |x| takes +1 at 0, and
 * a derivative that is not finite (sqrt at 0, log at 0) times a zero
 * sensitivity is 0, not NaN.
 *
 * **Tangent** (forward mode), in K directions, direction d being parameter
 * theta[direction_theta[d]]. The block computes what the source computes
 * and, beside each value, its K derivatives; it reads and writes the state's
 * tangents under tangent_tag(d). A derivative known to be zero when the
 * block is built (of a constant, a term, a draw, a comparison) is not
 * computed at all.
 *
 * **Adjoint** (reverse mode). The block recomputes the source's values from
 * the state the source started from (the kernel checkpoints it), then walks
 * the operations backwards: it reads the adjoints of what the source wrote
 * (State, kAdjoint), writes the adjoints of what it read, and adds the
 * parameters' adjoints (Theta, kAdjoint, accumulated; the local-vol grid's
 * through GridAccum). A region is reversed as a whole: an `if` becomes an
 * `if` on the same condition around the reverse of the block that ran, and
 * a fuzzy `if` two of them, one per block, with the blend's weights.
 *
 * Applying `tangent` to an adjoint block gives the second order (§5.3): a
 * row of the Hessian per direction.
 */

namespace quantModeling::scripting::ir
{

    /// The tangent of `b`. Throws InvalidInput on a script-level block, and
    /// on a direction that is a point of the local-vol grid (the adjoint is
    /// the mode for those).
    Block tangent(const Block &b, const std::vector<std::int32_t> &direction_theta);

    /// The adjoint of `b`. Parameters from `n_risks` on are not risks (a
    /// Cholesky factor): nothing is accumulated for them.
    Block adjoint(const Block &b, std::int32_t n_risks = 1 << 30);

    /// A path's blocks, each replaced by its derivative.
    PathProgram tangent(const PathProgram &p, const std::vector<std::int32_t> &direction_theta);
    PathProgram adjoint(const PathProgram &p);

    // ── paths on the host ──────────────────────────────────────────────────

    /// Scratch of a host path with derivatives.
    struct DerivativeScratch
    {
        std::vector<double> state, val, tangent, adjoint, adjoint_tangent, checkpoints, draws;
        std::vector<std::int32_t> record; ///< the blocks the forward sweep ran
    };

    /**
     * @brief One path of a tangent program (`tangent(p, directions)`): the
     *        payoff, and its derivative in each direction in `derivatives`.
     */
    double run_path_tangent(const PathProgram &tangent_program, int directions, const PathData &data,
                            const DeviceModel &model, const Script &script, mc::PathDraws &draws, double sign,
                            DerivativeScratch &scratch, double *derivatives);

    /**
     * @brief One path forward through `primal`, the state checkpointed
     *        before every block, then backward through `adjoint_program`
     *        (`adjoint(primal)`): the payoff, and `weight` times its
     *        gradient added to `gradient` (parameter p at
     *        gradient[p * stride]: the layout's thetas, then the grid).
     */
    double run_path_adjoint(const PathProgram &primal, const PathProgram &adjoint_program, const PathData &data,
                            const DeviceModel &model, const Script &script, mc::PathDraws &draws, double sign,
                            double weight, DerivativeScratch &scratch, double *gradient, long stride = 1);

    /**
     * @brief Second order (blueprint/wp/27-ir-ad.md §5.3, lot L6): one path
     *        forward through `forward` = tangent(primal, directions), state
     *        and tangents checkpointed, then backward through `backward` =
     *        tangent(adjoint(primal), directions).
     *
     * Returns the payoff; adds its gradient to `gradient[p]` and, for each
     * direction d, the row d of its Hessian to `hessian[p * directions + d]`
     * = d2 payoff / d theta_p d theta_(direction d). Exact where the payoff
     * is twice differentiable along the path; a kink (max, a hard or
     * linearly smoothed `if`) contributes nothing, which is the pathwise
     * second derivative's known blind spot (§8).
     */
    double run_path_second_order(const PathProgram &forward, const PathProgram &backward, int directions,
                                 const PathData &data, const DeviceModel &model, const Script &script,
                                 mc::PathDraws &draws, double sign, DerivativeScratch &scratch, double *gradient,
                                 double *hessian);

} // namespace quantModeling::scripting::ir

#endif // QM_SCRIPTING_IR_AD_HPP
