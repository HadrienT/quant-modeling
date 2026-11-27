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
 *     [ S_0 .. S_{n-1} | V (Heston) | script variables | payoff | F_0 v_0 .. ],
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
        /// Heston and SLV price kernels: the variance by QE, mc::qe_step
        /// (#317). Never with `risk`: QE's branches make its pathwise
        /// derivatives discontinuous, so the risks step by Euler.
        bool qe = false;
        int factors = 1; ///< gaussians a step reads

        // state
        int n_model = 1; ///< the model's slots: the spots, then the variance
        /// The script reads a last-step expectation (lastStepCall / Put /
        /// Digital, WP 27 §14.9): the step also leaves, per asset, the
        /// forward F and the log-variance v of the step it just took, and
        /// the events compute Black's formula from them.
        bool last_step = false;
        /// The script reads bridge survivals (survivalAbove / survivalBelow,
        /// WP 26 P2): n_surv running products, one per survival slot of
        /// the date the path is heading for, multiplied by every step's
        /// bridge factor and set back to 1 once the date has read them.
        int n_surv = 0;
        /// The script has knock-outs (knockOutAbove / Below, #369): the step
        /// into a knock-out date draws conditioned on survival -- the step
        /// row's flag and corridor say which steps and where -- and leaves
        /// the survival probability in the state slot ko(), 1 after any
        /// other step. One underlying.
        bool knock_out = false;
        int var0() const { return n_model; }
        int pay() const { return n_model + n_vars; }
        int last0() const { return n_model + n_vars + 1; } ///< F of asset a at last0() + 2a, v at + 1
        int surv0() const { return last0() + (last_step ? 2 * n_assets : 0); }
        int ko() const { return surv0() + n_surv; }
        int n_state() const { return ko() + (knock_out ? 1 : 0); }

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
        int step_cols = 0; ///< per step
        /// The step row's first survival column: level and "above" (1/0)
        /// of each survival slot of the date the step leads to.
        int surv_col0 = 0;
        /// With knock_out: the step row's columns flag (1 when the step
        /// reaches a knock-out date), has_lo, ln lo, has_hi, ln hi -- the
        /// corridor of that date, finite even where absent.
        int ko_col0 = -1;
        /// Multilevel's coarse path conditioned on the fine one (Giles &
        /// Szpruch 2012; WP 26 §26.2): the step also reads `draw(factors)`,
        /// the part a of its draw the fine path's first sub-step carries,
        /// and the step row's column `half_col`, that sub-step's share w of
        /// the step. Its last step's forward and variance, and its bridge
        /// survivals through the midpoint, are then conditional on a --
        /// LocalVolSimModel::condition_on_fine_half_steps's formulas. Local
        /// vol only; the kernel's driver builds z = sqrt(w) z1 + sqrt(1 - w)
        /// z2 and a = sqrt(w) z1 from two fine draws.
        bool half = false;
        int half_col = -1;
        /// Local vol, Milstein before the dates (#371): the step row's column
        /// `mil_col` is 1 on a step that reaches no product date, which then
        /// adds Milstein's term 1/2 sigma (S dsigma/dS) dt (z^2 - 1), as
        /// mc::local_vol_milstein_step; the steps into a date stay log-Euler.
        /// Price programs only.
        bool milstein = false;
        int mil_col = -1;
        /// WP 32 F4: the model steps on a forward curve; the step row's
        /// column `carry_col` is the curve's carry rate over the step, which
        /// the step adds to the rate (c_k + r, DeviceModel::step_carry) --
        /// every model of a risk program, local vol, Heston and SLV of a price
        /// one (Black-Scholes's price reads its tabulated drift). A price's
        /// date rows hold the curve's factors times the rate's; a risk's hold
        /// its times and maturities, and from `factor_col0` the curve's
        /// factors alone (1/DF(t), DF(T)/DF(t), F(T)/F(t)) in the same order.
        bool curve = false;
        int carry_col = -1;
        int factor_col0 = -1;
        /// WP 32: the risks per pillar of the forward curve, an adjoint
        /// program's (DeviceModel::curve_pillars). Thetas th_pillar0 ..
        /// + n_pillars - 1 are the pillar shifts, in CurveCarry's order,
        /// risks after the model's scalars (the grid's points follow them).
        /// Each step's rate and each date's factors (1/DF(t), DF(T)/DF(t),
        /// F(T)/F(t), by their column) add kPillarTerms shifts, as
        /// CurveShift(slot, coef) pairs read from the rows: a step's at
        /// pil_step_col0, a date factor f's at pil_event_col0 + 2 kPillarTerms f.
        static constexpr int kPillarTerms = 8;
        int n_pillars = 0;
        int th_pillar0 = -1;
        int pil_step_col0 = -1;
        int pil_event_col0 = -1;
        int n_factor_cols = 0; ///< the date factors' columns (factor_col0 of them)
        int event_cols = 1;    ///< per date: 1 + the most df() slots a date reads, + the most fwd() slots
        /// The date row's first fwd() column: tau = T - t (risk), or
        /// e^((r - q) tau) (price), q spot(0)'s dividend yield.
        int fwd_col0 = 1;
    };

    /// The layout of a script on a model kind; `max_df` from the script's
    /// blocks.
    /// `last_step`: leave the last step's forward and variance in the state
    /// even when the script reads no last-step expectation (Vibrato needs
    /// them, WP 27 §14.11).
    PathLayout path_layout(DeviceModel::Kind kind, int n_assets, const Script &script, bool risk,
                           bool last_step = false, bool half = false, bool milstein = false, bool curve = false,
                           int pillars = 0);

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
    /// The same, for a script: its bridge survivals need one underlying
    /// under Black-Scholes or local vol (the CPU models' own scope).
    std::string path_program_unsupported(const Script &script, DeviceModel::Kind kind, int n_assets);

    /// `qe`: Heston and SLV price programs step the variance by QE (#317);
    /// ignored with `risk` or for another model.
    /// `half`: a multilevel coarse path conditioned on the fine one
    /// (PathLayout::half); local vol only, refused otherwise.
    PathProgram path_program(const Script &script, DeviceModel::Kind kind, int n_assets, bool risk,
                             bool last_step = false, bool qe = false, bool half = false, bool milstein = false,
                             bool curve = false, int pillars = 0);

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
