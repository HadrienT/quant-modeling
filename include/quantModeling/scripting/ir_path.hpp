#ifndef QM_SCRIPTING_IR_PATH_HPP
#define QM_SCRIPTING_IR_PATH_HPP

#include <string>
#include <vector>

#include "quantModeling/engines/mc/path_draws.hpp"
#include "quantModeling/models/device_model.hpp"
#include "quantModeling/scripting/ir.hpp"

/**
 * @file ir_path.hpp
 * @brief A whole path as IR blocks: the model's step and the script's
 *        events on one state (blueprint/wp/27-ir-ad.md §3.3, lot L2).
 *
 * A path is three kinds of block run by a loop over the model's steps:
 *
 *     init                       the state at t = 0 from the parameters
 *     for each step:
 *         step                   the model's step (when the step draws)
 *         event[kind]            the script's statements (when it is a date)
 *
 * all reading and writing one array of *state* per path,
 *
 *     [ S_0 .. S_{n-1} | V (Heston) | script variables | payoff ],
 *
 * the model's *parameters* theta (what the derivatives are taken against),
 * and per step or per date a row of path-independent *data*.
 *
 * The step is written here once per model, with the IR builder, as the
 * hand-written step (models/equity/path_steps.hpp, engines/mc/script_path.hpp)
 * computes it, operation for operation: a path run from these blocks is
 * mc::script_path's, bit for bit (tests/testScriptIrPath.cpp). Every
 * derivative of the path then follows from the blocks (ir_ad.hpp); no
 * model has an adjoint written by hand.
 *
 * Two variants of the same path, as script_path has for double and Dual:
 *
 *  - `risk = false`, the price: Black-Scholes steps on the coefficients the
 *    CPU model tabulated (drift and vol sqrt(dt) per step), the numeraire and
 *    the discount factors read from the date's data row;
 *  - `risk = true`, what is differentiated: Black-Scholes steps on its
 *    parameters (rate, dividend, vol), the numeraire exp(r t) and the
 *    discount factors exp(-r tau) computed from the rate.
 */

namespace quantModeling::scripting::ir
{

    /// Where everything lives for one (model, script) pair.
    struct PathLayout
    {
        DeviceModel::Kind kind = DeviceModel::Kind::BlackScholes;
        int n_assets = 1;
        int n_vars = 0;
        bool risk = false;
        int factors = 1; ///< gaussians a step reads

        // state
        int n_model = 1; ///< the model's slots: the spots, then the variance
        int var0() const { return n_model; }
        int pay() const { return n_model + n_vars; }
        int n_state() const { return n_model + n_vars + 1; }

        // theta: the differentiable parameters first, in the CPU model's
        // label order (mc::dual_directions), then what is not differentiated
        // (the Cholesky factor), then the grid.
        int th_spot = 0, th_rate = 0, th_div = 0, th_vol = 0;
        int th_chol = -1;   ///< n_assets^2, row-major (several assets)
        int th_heston = -1; ///< v0, kappa, theta, xi, rho
        int th_grid = -1;   ///< the local-vol grid, K-major
        int n_theta = 0;    ///< without the grid
        int n_risks = 0;    ///< leading thetas that are risks (then the grid's points)

        // data rows
        int step_cols = 0;  ///< per step
        int event_cols = 1; ///< per date: 1 + the most df() slots a date reads
    };

    /// The layout of a script on a model kind; `max_df` from the script's
    /// blocks.
    PathLayout path_layout(DeviceModel::Kind kind, int n_assets, const Script &script, bool risk);

    /// The whole program.
    struct PathProgram
    {
        PathLayout layout;
        Block init;                ///< state at t = 0: reads theta and the baseline (its data row)
        Block step;                ///< one model step
        std::vector<Block> events; ///< the script's distinct blocks, bound to the state
    };

    /// "" when `path_program` covers the model, else why not.
    std::string path_program_unsupported(DeviceModel::Kind kind, int n_assets);

    PathProgram path_program(const Script &script, DeviceModel::Kind kind, int n_assets, bool risk);

    /// A script-level block bound to the path's state and data (VarIn, PayIn
    /// and Spot become State; Num and Df the date's data, or exp of the rate
    /// when `layout.risk`).
    Block bind_event(const Block &event, const PathLayout &layout);

    /// What the blocks read besides the state, for one request (host
    /// memory; the GPU copies it).
    struct PathData
    {
        std::vector<double> theta;      ///< layout.n_theta, then the grid
        std::vector<double> step_rows;  ///< n_steps x step_cols
        std::vector<double> event_rows; ///< n_events x event_cols
        std::vector<double> baseline;   ///< n_vars (the init block's data row)
        std::vector<std::int32_t> term_begin;
        std::vector<double> terms; ///< every event's terms, one after the other
    };

    /**
     * @brief The data of a request. The price variant's date rows hold
     *        exp(r t) and exp(-r tau), computed here with the host's exp:
     *        the GPU, whose exp differs in the last bit, fills them on the
     *        device instead (src/gpu/script.cu).
     */
    PathData path_data(const PathLayout &layout, const DeviceModel &model, const Script &script,
                       const std::vector<std::vector<Time>> &discount_mats, const std::vector<Real> &baseline);

    /// Scratch of a host path: the state, the values of the longest block.
    struct PathScratch
    {
        std::vector<double> state, val;
        explicit PathScratch(const PathProgram &p);
    };

    /**
     * @brief One path on the host, from the blocks -- mc::script_path's loop:
     *        the reference of the generated kernels, and the CPU's path
     *        where the IR runs it. Returns the (deflated) payoff; the final
     *        state is left in `scratch.state`.
     */
    double run_path(const PathProgram &p, const PathData &data, const DeviceModel &model, const Script &script,
                    mc::PathDraws &draws, double sign, PathScratch &scratch);

} // namespace quantModeling::scripting::ir

#endif // QM_SCRIPTING_IR_PATH_HPP
