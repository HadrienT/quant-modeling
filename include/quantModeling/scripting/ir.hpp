#ifndef QM_SCRIPTING_IR_HPP
#define QM_SCRIPTING_IR_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/scripting/event.hpp"

/**
 * @file ir.hpp
 * @brief The intermediate representation of a script and of a model's step
 *        (blueprint/wp/27-ir-ad.md §3, lots L1-L4).
 *
 * A `Block` is a list of instructions in static single assignment form:
 * instruction k defines value k, once, and its operands `a`, `b`, `c` are the
 * indices of earlier instructions. What a block reads from outside is an
 * instruction too, and what it leaves is listed in `outs`.
 *
 * Two levels of inputs:
 *
 *  - *script level*, as the lowering of a script's event writes them: the
 *    script's variables as the date starts (`VarIn`), the payoff so far
 *    (`PayIn`), the spots, discount factors, survival probabilities and
 *    numeraire of the date (`Spot`, `Df`, `Surv`, `Num`), the product's
 *    terms (`Param`);
 *
 *  - *path level*, what a kernel that simulates a path gives a block
 *    (ir_path.hpp binds a script's block to it, and builds the models'
 *    steps): one array of state per path (`State`: the model's state, then
 *    the script's variables, then the payoff), the step's gaussians
 *    (`Draw`), the step's or the date's path-independent numbers (`Data`),
 *    the model's parameters (`Theta`), the terms (`Param`).
 *
 * Control flow stays structured (§3.2):
 *
 *     If c            then-block when value c is not 0
 *       ...
 *     Else
 *       ...
 *     EndIf
 *     Phi t, e        one per value the two blocks leave different
 *
 * and the fuzzy `if` of the smoothed scripts, whose degree d picks a mode
 * the way the bytecode interpreter does -- d <= 0 the else-block alone,
 * d >= 1 the then-block alone, in between both, blended:
 *
 *     FuzzyIf d
 *       ...           (reads the values as they were before the if)
 *     FuzzyElse
 *       ...           (reads the same values: SSA needs no save / restore)
 *     FuzzyEnd
 *     FuzzyPhi t, e   d t + (1 - d) e when blended, else the block that ran
 *     FuzzyPhiPay t, e
 *
 * A block's values leave it through the phis. An adjoint block also reads
 * them from a later region opened on the *same condition* (its reverse
 * sweep), which is sound because that region runs exactly when the first
 * did. A value of a block that did not run reads as 0 (the evaluator and
 * the generated code initialise every value), so that a `Select` may name
 * it.
 *
 * Derivatives are blocks too (ir_ad.hpp): the tangent and the adjoint of a
 * block are built from it by a transformation, and read and write the same
 * slots under a *tag* -- the tangent in direction d of state slot i, the
 * adjoint of state slot i, the adjoint of parameter p.
 *
 * The bits rule (§4): the lowering emits the operations of the bytecode
 * interpreter (scripting/bytecode.hpp) in the same order, and the passes
 * never reorder, reassociate or contract them. Common subexpressions and
 * dead code go; constants fold only through operations that IEEE 754 rounds
 * correctly (+ - * / sqrt), never through exp, log or pow, whose last bit
 * differs from one library to the next. A price computed from the IR is the
 * interpreter's, bit for bit (tests/testScriptIr.cpp on the CPU,
 * tests/gpu/testGpuScriptJit.cpp on the device).
 */

namespace quantModeling::scripting::ir
{

    enum class Op : std::uint8_t
    {
        // ── inputs, script level ────────────────────────────────────────────
        Const, ///< x
        Param, ///< term a of the event: a number of the script passed as data (lot L1b)
        VarIn, ///< script variable a as the event starts
        PayIn, ///< the payoff as the event starts
        Spot,  ///< spot a of the date
        Df,    ///< discount slot a of the date
        Surv,  ///< survival slot a of the date
        Num,   ///< the date's numeraire
        // ── inputs, path level ──────────────────────────────────────────────
        State, ///< slot a of the path's state; c: the tag (primal, a tangent, the adjoint)
        Draw,  ///< gaussian a of the step
        Data,  ///< number a of the step's (or the date's) data row: not differentiated
        Theta, ///< model parameter a: what the derivatives are taken against
        // ── arithmetic and functions ────────────────────────────────────────
        Add,
        Sub,
        Mul,
        Div,
        Pow,
        Neg,
        Min, ///< (b < a) ? b : a  -- std::min
        Max, ///< (a < b) ? b : a  -- std::max
        Log,
        Exp,
        Sqrt,
        Abs,
        Smooth, ///< smooth(a, b): b <= 0 ? [a > 0] : clamp01((a + b) / (b + b))
        // ── crisp logic: 1.0 or 0.0 ─────────────────────────────────────────
        Eq,
        Ne,
        Gt,
        Ge,
        Lt,
        Le,
        And,
        Or,
        Not,
        Select, ///< a != 0 ? b : c
        // ── fuzzy degrees ───────────────────────────────────────────────────
        Up,   ///< clamp01((a + x) / (2 x)): the degree of a > 0, half-width x
        Tent, ///< clamp01(1 - |a| / x): the degree of a = 0
        // ── the model's grid (local vol): parameters Theta[grid0 ...] ───────
        GridVol,   ///< the local vol at spot a and time b: bilinear, floored (mc::local_vol_bilinear)
        GridSlope, ///< its slope in the spot (0 outside the strikes and where the floor binds)
        GridAccum, ///< a statement: adjoint a of the vol at (spot b, time c), added to its four corners
        // ── regions ─────────────────────────────────────────────────────────
        If,          ///< a: condition; b: its Else; c: its EndIf
        Else,        ///< a: its If; c: its EndIf
        EndIf,       ///< a: its If
        Phi,         ///< a: then-value, b: else-value, c: the If
        FuzzyIf,     ///< a: degree; b: the payoff before; c: its FuzzyElse
        FuzzyElse,   ///< a: its FuzzyIf; c: its FuzzyEnd
        FuzzyEnd,    ///< a: its FuzzyIf
        FuzzyPhi,    ///< a: then-value, b: else-value, c: the FuzzyIf
        FuzzyPhiPay, ///< the payoff's blend: a, b, c as FuzzyPhi
    };

    struct Inst
    {
        Op op = Op::Const;
        std::int32_t a = -1;
        std::int32_t b = -1;
        std::int32_t c = -1;
        double x = 0.0;

        bool operator==(const Inst &o) const
        {
            return op == o.op && a == o.a && b == o.b && c == o.c && std::memcmp(&x, &o.x, sizeof x) == 0;
        }
    };

    // ── tags: which copy of a slot an instruction or an output names ───────

    inline constexpr std::int32_t kPrimal = 0;
    /// The tangent in direction d: 1 + d.
    constexpr std::int32_t tangent_tag(int d)
    {
        return 1 + d;
    }
    inline constexpr std::int32_t kAdjoint = 1 << 16;
    /// The tangent in direction d of the adjoint (second order).
    constexpr std::int32_t adjoint_tangent_tag(int d)
    {
        return kAdjoint + 1 + d;
    }
    constexpr bool is_adjoint(std::int32_t tag)
    {
        return tag >= kAdjoint;
    }
    /// The direction of a tangent tag, -1 for the primal and the adjoint.
    constexpr int direction_of(std::int32_t tag)
    {
        return (tag & (kAdjoint - 1)) - 1;
    }

    /// Whether `op` defines a number (the region markers and GridAccum do not).
    constexpr bool defines_value(Op op)
    {
        return op != Op::If && op != Op::Else && op != Op::EndIf && op != Op::FuzzyIf && op != Op::FuzzyElse &&
               op != Op::FuzzyEnd && op != Op::GridAccum;
    }

    /// Operands of `op` that are values: bit 0 for a, 1 for b, 2 for c.
    constexpr unsigned value_operands(Op op)
    {
        switch (op)
        {
            case Op::Const:
            case Op::Param:
            case Op::VarIn:
            case Op::PayIn:
            case Op::Spot:
            case Op::Df:
            case Op::Surv:
            case Op::Num:
            case Op::State:
            case Op::Draw:
            case Op::Data:
            case Op::Theta:
            case Op::Else:
            case Op::EndIf:
            case Op::FuzzyElse:
            case Op::FuzzyEnd:
                return 0u;
            case Op::Neg:
            case Op::Log:
            case Op::Exp:
            case Op::Sqrt:
            case Op::Abs:
            case Op::Not:
            case Op::Up:
            case Op::Tent:
            case Op::If:
                return 1u;
            case Op::Select:
            case Op::GridAccum:
                return 7u;
            default: // binary operations, the phis, FuzzyIf (degree, payoff before or -1)
                return 3u;
        }
    }

    /// Fields of `op` that name another instruction of the block without
    /// reading its value (a region's markers): the same bits.
    constexpr unsigned link_operands(Op op)
    {
        switch (op)
        {
            case Op::If:
                return 6u;
            case Op::Else:
            case Op::FuzzyElse:
                return 5u;
            case Op::EndIf:
            case Op::FuzzyEnd:
                return 1u;
            case Op::FuzzyIf:
            case Op::Phi:
            case Op::FuzzyPhi:
            case Op::FuzzyPhiPay:
                return 4u;
            default:
                return 0u;
        }
    }

    /// What a block leaves: `value` written to slot `index` of `cls` under
    /// `tag`. cls is VarIn or PayIn (script level), State, or Theta (an
    /// adjoint, accumulated: `add`).
    struct Out
    {
        Op cls = Op::State;
        std::int32_t index = 0;
        std::int32_t tag = kPrimal;
        std::int32_t value = -1;
        bool add = false; ///< slot += value rather than slot = value

        bool operator==(const Out &o) const
        {
            return cls == o.cls && index == o.index && tag == o.tag && value == o.value && add == o.add;
        }
    };

    /// One event's statements, a model's step, or a derivative of either.
    struct Block
    {
        std::vector<Inst> code;
        std::vector<Out> outs;
        /// Highest spot index / discount slot / survival slot read (-1: none).
        std::int32_t max_spot = -1, max_df = -1, max_surv = -1;

        bool operator==(const Block &o) const { return code == o.code && outs == o.outs; }

        /// The value a script-level block leaves in variable i (-1: as it
        /// was), and in the payoff.
        std::int32_t var_out(std::int32_t i) const
        {
            for (const Out &o : outs)
                if (o.cls == Op::VarIn && o.index == i && o.tag == kPrimal)
                    return o.value;
            return -1;
        }
        std::int32_t pay_out() const
        {
            for (const Out &o : outs)
                if (o.cls == Op::PayIn && o.tag == kPrimal)
                    return o.value;
            return -1;
        }
    };

    /// A script: its distinct blocks, and which one each event runs (the
    /// dates of a schedule share theirs).
    struct Script
    {
        std::vector<Block> blocks;
        std::vector<std::int32_t> event_block; ///< per event: index into `blocks`
        /// Lot L1b, a script lowered with its numbers as terms: per event,
        /// the values its block's `Param` instructions read, in order. Two
        /// events (or two products) that differ only by their numbers then
        /// share a block, and the code compiled from it. Empty lists
        /// otherwise: the numbers are constants of the blocks.
        std::vector<std::vector<double>> event_terms;
        int n_vars = 0;
        bool fuzzy = false;
        bool has_survival = false;

        std::size_t n_events() const { return event_block.size(); }
        const Block &event(std::size_t e) const { return blocks[static_cast<std::size_t>(event_block[e])]; }
        bool empty() const { return blocks.empty(); }
    };

    /**
     * @brief Appends instructions to a block, sharing what is already there.
     *
     * `value()` returns an existing instruction when the same operation on
     * the same operands is visible from the current region (common
     * subexpressions), and folds an operation on constants when the fold is
     * exact. `raw()` always appends (the region markers and the phis).
     */
    class Builder
    {
      public:
        Builder() { scopes_.emplace_back(); }

        std::int32_t value(Op op, std::int32_t a = -1, std::int32_t b = -1, double x = 0.0, std::int32_t c = -1);
        std::int32_t constant(double x) { return value(Op::Const, -1, -1, x); }
        std::int32_t raw(Op op, std::int32_t a = -1, std::int32_t b = -1, std::int32_t c = -1, double x = 0.0);

        // Shorthands for the arithmetic the transformations and the models write.
        std::int32_t add(std::int32_t a, std::int32_t b) { return value(Op::Add, a, b); }
        std::int32_t sub(std::int32_t a, std::int32_t b) { return value(Op::Sub, a, b); }
        std::int32_t mul(std::int32_t a, std::int32_t b) { return value(Op::Mul, a, b); }
        std::int32_t div(std::int32_t a, std::int32_t b) { return value(Op::Div, a, b); }
        std::int32_t neg(std::int32_t a) { return value(Op::Neg, a); }
        std::int32_t select(std::int32_t cond, std::int32_t then_value, std::int32_t else_value)
        {
            return value(Op::Select, cond, then_value, 0.0, else_value);
        }
        /// A path-level input: State (with its tag), Draw, Data, Theta.
        std::int32_t input(Op cls, std::int32_t index, std::int32_t tag = kPrimal)
        {
            return value(cls, index, -1, 0.0, cls == Op::State ? tag : -1);
        }

        void open_scope();
        void close_scope() { scopes_.pop_back(); }
        int depth() const { return static_cast<int>(scopes_.size()) - 1; }

        Inst &at(std::int32_t i) { return block_.code[static_cast<std::size_t>(i)]; }
        Block &block() { return block_; }
        Block take() { return std::move(block_); }

      private:
        using Key = std::tuple<int, std::int32_t, std::int32_t, std::int32_t, std::uint64_t>;
        Block block_;
        std::vector<std::map<Key, std::int32_t>> scopes_;
    };

    /**
     * @brief Copies a block instruction by instruction into a builder, the
     *        operands renamed: what the binding and the derivative
     *        transformations are written on. The caller walks the source in
     *        order and, for each instruction, either calls `copy(k)` or
     *        emits a replacement and records it in `map[k]`.
     */
    class Rebuilder
    {
      public:
        explicit Rebuilder(const Block &src)
            : map(src.code.size(), -1), src_(src) {}

        Builder b;
        std::vector<std::int32_t> map; ///< source instruction -> its copy

        std::int32_t copy(std::int32_t k);
        std::int32_t operator[](std::int32_t k) const { return k < 0 ? -1 : map[static_cast<std::size_t>(k)]; }

      private:
        const Block &src_;
    };

    /// Removes what no output reads (values, and whole regions none of whose
    /// phis is read); the survivors keep their order.
    void remove_dead_code(Block &b);

    /**
     * @brief The script's events as IR, from the tree the front-end passes
     *        left (variable indexing, domains, df and barrier lookups) --
     *        what scripting::compile_script reads.
     *
     * `fuzzy` selects the smoothed semantics. Throws InvalidInput where the
     * bytecode compiler does (exercise(), a comparison used as a number in
     * fuzzy mode).
     *
     * `terms`: every number written in the script becomes a `Param` (lot
     * L1b), its value in Script::event_terms -- for the scripts the library
     * writes itself, whose numbers are a product's terms. The same doubles
     * reach the same operations, so the bits do not move; only the folds of
     * constants are left to run time.
     */
    Script lower_script(const std::vector<Event> &events, std::size_t n_vars, bool fuzzy, bool terms = false);

    /// A block as text, one instruction per line (`%7 = add %3, %6`).
    std::string to_string(const Block &b);
    std::string to_string(const Script &s);

    // ── evaluation on the CPU ──────────────────────────────────────────────

    namespace detail
    {
        inline double clamp01(double v)
        {
            if (v <= 0.0)
                return 0.0;
            if (v >= 1.0)
                return 1.0;
            return v;
        }

        enum Mode : int
        {
            kThenOnly = 0,
            kElseOnly = 1,
            kBlend = 2
        };

        inline int fuzzy_mode(double degree)
        {
            return degree <= 0.0 ? kElseOnly : (degree >= 1.0 ? kThenOnly : kBlend);
        }
    } // namespace detail

    /**
     * @brief What a block runs against: the memory behind each class of
     *        input and output. A null pointer is a class the block does not
     *        use.
     */
    struct Frame
    {
        // script level
        double *vars = nullptr;
        double *payoff = nullptr;
        const double *spots = nullptr;
        const double *discounts = nullptr;
        const double *survivals = nullptr;
        double numeraire = 1.0;
        const double *terms = nullptr;
        // path level
        double *state = nullptr;           ///< primal state (an adjoint block reads the checkpointed one)
        double *state_tangent = nullptr;   ///< [slot * directions + d]
        double *state_adjoint = nullptr;   ///< [slot]
        double *adjoint_tangent = nullptr; ///< [slot * directions + d]
        int directions = 0;
        const double *draws = nullptr;
        const double *data = nullptr;
        const double *theta = nullptr;
        double *theta_adjoint = nullptr; ///< parameter p at [p * theta_stride]
        long theta_stride = 1;
        double *theta_adjoint_tangent = nullptr; ///< [p * directions + d]
        /// The local-vol grid: strikes and times; its values are
        /// theta[grid0 + i * nT + j].
        const Real *grid_K = nullptr;
        int grid_nK = 0;
        const Real *grid_T = nullptr;
        int grid_nT = 0;
        int grid0 = 0;

        mc::GridView<Real> grid() const { return {grid_K, grid_nK, grid_T, grid_nT, theta + grid0}; }
    };

    namespace detail
    {
        /// The four corners and weights of the local vol at (S, t), and
        /// whether the floor leaves it free (mc::local_vol_bilinear's
        /// arithmetic; what GridSlope and GridAccum need).
        struct GridCell
        {
            int i0, i1, j0, j1;
            double wK, wT, dK;
            bool inside, free;
        };

        QM_HOST_DEVICE inline GridCell grid_cell(const mc::GridView<Real> &g, double S, double t)
        {
            GridCell c{};
            const double Sc = S < g.K[0] ? g.K[0] : (S > g.K[g.nK - 1] ? g.K[g.nK - 1] : S);
            c.inside = Sc == S;
            mc::bracket(g.K, g.nK, Sc, c.i0, c.i1);
            const double K0 = g.K[c.i0];
            c.dK = g.K[c.i1] - K0;
            c.wK = c.dK <= 1e-12 ? 0.0 : (c.inside ? (S - K0) / c.dK : (Sc - K0) / c.dK);
            const double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
            mc::bracket(g.T_grid, g.nT, tc, c.j0, c.j1);
            const double T0 = g.T_grid[c.j0];
            const double dT = g.T_grid[c.j1] - T0;
            c.wT = (dT > 1e-12) ? (tc - T0) / dT : 0.0;
            const double sigma = (1.0 - c.wK) * (1.0 - c.wT) * g.at(c.i0, c.j0) +
                                 c.wK * (1.0 - c.wT) * g.at(c.i1, c.j0) + (1.0 - c.wK) * c.wT * g.at(c.i0, c.j1) +
                                 c.wK * c.wT * g.at(c.i1, c.j1);
            c.free = sigma > 1e-6;
            return c;
        }

        QM_HOST_DEVICE inline double grid_slope(const mc::GridView<Real> &g, double S, double t)
        {
            const GridCell c = grid_cell(g, S, t);
            if (!c.free || !c.inside || c.dK <= 1e-12)
                return 0.0;
            return ((1.0 - c.wT) * (g.at(c.i1, c.j0) - g.at(c.i0, c.j0)) +
                    c.wT * (g.at(c.i1, c.j1) - g.at(c.i0, c.j1))) /
                   c.dK;
        }

        /// `a` times each corner's weight, added to the corners' adjoints
        /// (parameter p at base[p * stride]).
        QM_HOST_DEVICE inline void grid_accumulate(const mc::GridView<Real> &g, double S, double t, double a,
                                                   double *base, long stride, int grid0)
        {
            const GridCell c = grid_cell(g, S, t);
            if (!c.free)
                return;
            const long nT = g.nT;
            base[(grid0 + c.i0 * nT + c.j0) * stride] += a * ((1.0 - c.wK) * (1.0 - c.wT));
            base[(grid0 + c.i1 * nT + c.j0) * stride] += a * (c.wK * (1.0 - c.wT));
            base[(grid0 + c.i0 * nT + c.j1) * stride] += a * ((1.0 - c.wK) * c.wT);
            base[(grid0 + c.i1 * nT + c.j1) * stride] += a * (c.wK * c.wT);
        }
    } // namespace detail

    /**
     * @brief Run one block on one path: `val` (b.code.size() doubles of
     *        scratch) receives every value; the outputs are written to the
     *        frame once every instruction has run.
     *
     * The reference implementation of the IR's semantics: each operation is
     * written as the bytecode interpreter and the models' steps write it.
     */
    inline void run_block(const Block &b, const Frame &f, double *val)
    {
        using namespace detail;
        const auto n = static_cast<std::int32_t>(b.code.size());
        for (std::int32_t k = 0; k < n; ++k)
            val[k] = 0.0;
        // A fuzzy if's mode lives in its FuzzyIf's own slot of `val`.
        const auto mode_of = [&](std::int32_t fuzzy_if)
        { return static_cast<int>(val[fuzzy_if]); };
        for (std::int32_t k = 0; k < n; ++k)
        {
            const Inst &in = b.code[static_cast<std::size_t>(k)];
            const unsigned operands = value_operands(in.op);
            const double a = (operands & 1u) && in.a >= 0 ? val[in.a] : 0.0;
            const double c = (operands & 2u) && in.b >= 0 ? val[in.b] : 0.0; // the second operand
            double r = 0.0;
            switch (in.op)
            {
                case Op::Const:
                    r = in.x;
                    break;
                case Op::Param:
                    r = f.terms[in.a];
                    break;
                case Op::VarIn:
                    r = f.vars[in.a];
                    break;
                case Op::PayIn:
                    r = *f.payoff;
                    break;
                case Op::Spot:
                    r = f.spots[in.a];
                    break;
                case Op::Df:
                    r = f.discounts[in.a];
                    break;
                case Op::Surv:
                    r = f.survivals[in.a];
                    break;
                case Op::Num:
                    r = f.numeraire;
                    break;
                case Op::State:
                {
                    const int d = direction_of(in.c);
                    if (!is_adjoint(in.c))
                        r = d < 0 ? f.state[in.a] : f.state_tangent[in.a * f.directions + d];
                    else
                        r = d < 0 ? f.state_adjoint[in.a] : f.adjoint_tangent[in.a * f.directions + d];
                    break;
                }
                case Op::Draw:
                    r = f.draws[in.a];
                    break;
                case Op::Data:
                    r = f.data[in.a];
                    break;
                case Op::Theta:
                    r = f.theta[in.a];
                    break;
                case Op::Add:
                    r = a + c;
                    break;
                case Op::Sub:
                    r = a - c;
                    break;
                case Op::Mul:
                    r = a * c;
                    break;
                case Op::Div:
                    r = a / c;
                    break;
                case Op::Pow:
                    r = std::pow(a, c);
                    break;
                case Op::Neg:
                    r = -a;
                    break;
                case Op::Min:
                    r = (c < a) ? c : a;
                    break;
                case Op::Max:
                    r = (a < c) ? c : a;
                    break;
                case Op::Log:
                    r = std::log(a);
                    break;
                case Op::Exp:
                    r = std::exp(a);
                    break;
                case Op::Sqrt:
                    r = std::sqrt(a);
                    break;
                case Op::Abs:
                    r = std::fabs(a);
                    break;
                case Op::Smooth:
                    if (c <= 0.0)
                        r = a > 0.0 ? 1.0 : 0.0;
                    else
                        r = clamp01((a + c) / (c + c));
                    break;
                case Op::Eq:
                    r = a == c ? 1.0 : 0.0;
                    break;
                case Op::Ne:
                    r = a != c ? 1.0 : 0.0;
                    break;
                case Op::Gt:
                    r = a > c ? 1.0 : 0.0;
                    break;
                case Op::Ge:
                    r = a >= c ? 1.0 : 0.0;
                    break;
                case Op::Lt:
                    r = a < c ? 1.0 : 0.0;
                    break;
                case Op::Le:
                    r = a <= c ? 1.0 : 0.0;
                    break;
                case Op::And:
                    r = (a != 0.0) && (c != 0.0) ? 1.0 : 0.0;
                    break;
                case Op::Or:
                    r = (a != 0.0) || (c != 0.0) ? 1.0 : 0.0;
                    break;
                case Op::Not:
                    r = (a != 0.0) ? 0.0 : 1.0;
                    break;
                case Op::Select:
                    r = a != 0.0 ? c : val[in.c];
                    break;
                case Op::Up:
                    r = clamp01((a + in.x) / (2.0 * in.x));
                    break;
                case Op::Tent:
                    r = clamp01(1.0 - std::fabs(a) / in.x);
                    break;

                case Op::GridVol:
                    r = mc::local_vol_bilinear<Real>(f.grid(), a, c);
                    break;
                case Op::GridSlope:
                    r = grid_slope(f.grid(), a, c);
                    break;
                case Op::GridAccum: // adjoint a of the vol at (spot b, time c)
                    grid_accumulate(f.grid(), c, val[in.c], a, f.theta_adjoint, f.theta_stride, f.grid0);
                    continue;

                case Op::If:
                    if (a == 0.0)
                        k = in.b; // past the Else
                    continue;
                case Op::Else: // the then-block ran
                    k = in.c;
                    continue;
                case Op::EndIf:
                    continue;
                case Op::Phi:
                    r = val[b.code[static_cast<std::size_t>(in.c)].a] != 0.0 ? a : c;
                    break;

                case Op::FuzzyIf:
                {
                    const int mode = fuzzy_mode(a);
                    val[k] = mode;
                    if (mode == kElseOnly)
                        k = in.c; // past the FuzzyElse
                    continue;
                }
                case Op::FuzzyElse:
                    if (mode_of(in.a) == kThenOnly)
                        k = in.c; // past the FuzzyEnd
                    continue;
                case Op::FuzzyEnd:
                    continue;
                case Op::FuzzyPhi:
                {
                    const int closed = mode_of(in.c);
                    if (closed == kThenOnly)
                        r = a;
                    else if (closed == kElseOnly)
                        r = c;
                    else
                    {
                        const double degree = val[b.code[static_cast<std::size_t>(in.c)].a];
                        const double w1 = 1.0 - degree;
                        r = degree * a + w1 * c;
                    }
                    break;
                }
                case Op::FuzzyPhiPay:
                {
                    const int closed = mode_of(in.c);
                    if (closed == kThenOnly)
                        r = a;
                    else if (closed == kElseOnly)
                        r = c;
                    else
                    {
                        const Inst &fi = b.code[static_cast<std::size_t>(in.c)];
                        const double degree = val[fi.a];
                        const double payoff0 = val[fi.b];
                        const double w1 = 1.0 - degree;
                        r = payoff0 + degree * (a - payoff0) + w1 * (c - payoff0);
                    }
                    break;
                }
            }
            val[k] = r;
        }
        // Outputs last: every input was read from the state before.
        for (const Out &o : b.outs)
        {
            const double v = val[o.value];
            const int d = direction_of(o.tag);
            double *slot = nullptr;
            switch (o.cls)
            {
                case Op::VarIn:
                    slot = &f.vars[o.index];
                    break;
                case Op::PayIn:
                    slot = f.payoff;
                    break;
                case Op::State:
                    if (!is_adjoint(o.tag))
                        slot = d < 0 ? &f.state[o.index] : &f.state_tangent[o.index * f.directions + d];
                    else
                        slot = d < 0 ? &f.state_adjoint[o.index] : &f.adjoint_tangent[o.index * f.directions + d];
                    break;
                default: // Theta: an adjoint
                    slot = d < 0 ? &f.theta_adjoint[o.index * f.theta_stride]
                                 : &f.theta_adjoint_tangent[o.index * f.directions + d];
                    break;
            }
            if (o.add)
                *slot += v;
            else
                *slot = v;
        }
    }

    /// A script-level block on one path: `vars` and `payoff` updated as the
    /// event leaves them.
    inline void run_block(const Block &b, double *vars, const double *spots, const double *discounts,
                          double numeraire, double &payoff, const double *survivals, double *val,
                          const double *terms = nullptr)
    {
        Frame f;
        f.vars = vars;
        f.payoff = &payoff;
        f.spots = spots;
        f.discounts = discounts;
        f.survivals = survivals;
        f.numeraire = numeraire;
        f.terms = terms;
        run_block(b, f, val);
    }

} // namespace quantModeling::scripting::ir

#endif // QM_SCRIPTING_IR_HPP
