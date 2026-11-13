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
#include "quantModeling/scripting/event.hpp"

/**
 * @file ir.hpp
 * @brief The intermediate representation of a script
 *        (blueprint/wp/27-ir-ad.md §3, lot L1).
 *
 * One event (a date's statements) is a `Block`: a list of instructions in
 * static single assignment form. Instruction k defines value k, once; its
 * operands `a`, `b`, `c` are the indices of earlier instructions. What an
 * event reads from outside -- the script's variables as the date starts, the
 * payoff so far, the spots, discount factors, survival probabilities and
 * numeraire of the date -- are instructions too (`VarIn`, `PayIn`, `Spot`,
 * ...), and what it leaves is named by `var_out` and `pay_out`.
 *
 * Control flow stays structured (§3.2):
 *
 *     If c            then-block when value c is not 0
 *       ...
 *     Else
 *       ...
 *     EndIf
 *     Phi t, e        one per variable the two blocks leave different
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
 * A block's values are only visible inside it; the phis are how they leave.
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
        // ── inputs ──────────────────────────────────────────────────────────
        Const, ///< x
        Param, ///< kernel parameter a (a product term: lot L1b), value x at lowering time
        VarIn, ///< script variable a as the event starts
        PayIn, ///< the payoff as the event starts
        Spot,  ///< spot a of the date
        Df,    ///< discount slot a of the date
        Surv,  ///< survival slot a of the date
        Num,   ///< the date's numeraire
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
        // ── fuzzy degrees ───────────────────────────────────────────────────
        Up,   ///< clamp01((a + x) / (2 x)): the degree of a > 0, half-width x
        Tent, ///< clamp01(1 - |a| / x): the degree of a = 0
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

    /// Whether `op` defines a number (the region markers do not).
    constexpr bool defines_value(Op op)
    {
        return op != Op::If && op != Op::Else && op != Op::EndIf && op != Op::FuzzyIf && op != Op::FuzzyElse &&
               op != Op::FuzzyEnd;
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
            case Op::FuzzyIf: // degree, payoff before (b may be -1: no payoff blend)
                return 3u;
            default: // binary operations and the phis
                return 3u;
        }
    }

    /// One event's statements.
    struct Block
    {
        std::vector<Inst> code;
        /// Per script variable: the value it holds as the event ends, or -1
        /// when the event leaves it as it was.
        std::vector<std::int32_t> var_out;
        std::int32_t pay_out = -1; ///< the payoff as the event ends; -1: unchanged
        /// Highest spot index / discount slot / survival slot read (-1: none).
        std::int32_t max_spot = -1, max_df = -1, max_surv = -1;

        bool operator==(const Block &o) const { return code == o.code && var_out == o.var_out && pay_out == o.pay_out; }
    };

    /// A script: its distinct blocks, and which one each event runs (the
    /// dates of a schedule share theirs).
    struct Script
    {
        std::vector<Block> blocks;
        std::vector<std::int32_t> event_block; ///< per event: index into `blocks`
        int n_vars = 0;
        int n_params = 0; ///< kernel parameters the blocks read (lot L1b)
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

        std::int32_t value(Op op, std::int32_t a = -1, std::int32_t b = -1, double x = 0.0);
        std::int32_t constant(double x) { return value(Op::Const, -1, -1, x); }
        std::int32_t raw(Op op, std::int32_t a = -1, std::int32_t b = -1, std::int32_t c = -1, double x = 0.0);

        void open_scope();
        void close_scope() { scopes_.pop_back(); }
        int depth() const { return static_cast<int>(scopes_.size()) - 1; }

        Inst &at(std::int32_t i) { return block_.code[static_cast<std::size_t>(i)]; }
        Block &block() { return block_; }
        Block take() { return std::move(block_); }

      private:
        using Key = std::tuple<int, std::int32_t, std::int32_t, std::uint64_t>;
        Block block_;
        std::vector<std::map<Key, std::int32_t>> scopes_;
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
     */
    Script lower_script(const std::vector<Event> &events, std::size_t n_vars, bool fuzzy);

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
     * @brief Run one block on one path: `val` (b.code.size() doubles of
     *        scratch) receives every value; `vars` and `payoff` are updated
     *        as the event leaves them.
     *
     * The reference implementation of the IR's semantics: each operation is
     * written as the bytecode interpreter writes it.
     */
    inline void run_block(const Block &b, double *vars, const double *spots, const double *discounts,
                          double numeraire, double &payoff, const double *survivals, double *val,
                          const double *params = nullptr)
    {
        using namespace detail;
        // A fuzzy if's mode lives in its FuzzyIf's own slot of `val`.
        const auto mode_of = [&](std::int32_t fuzzy_if)
        { return static_cast<int>(val[fuzzy_if]); };
        const auto n = static_cast<std::int32_t>(b.code.size());
        for (std::int32_t k = 0; k < n; ++k)
        {
            const Inst &in = b.code[static_cast<std::size_t>(k)];
            const double a = in.a >= 0 ? val[in.a] : 0.0;
            const double c = in.b >= 0 ? val[in.b] : 0.0; // the second operand
            double r = 0.0;
            switch (in.op)
            {
                case Op::Const:
                    r = in.x;
                    break;
                case Op::Param:
                    r = params ? params[in.a] : in.x;
                    break;
                case Op::VarIn:
                    r = vars[in.a];
                    break;
                case Op::PayIn:
                    r = payoff;
                    break;
                case Op::Spot:
                    r = spots[in.a];
                    break;
                case Op::Df:
                    r = discounts[in.a];
                    break;
                case Op::Surv:
                    r = survivals[in.a];
                    break;
                case Op::Num:
                    r = numeraire;
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
                case Op::Up:
                    r = clamp01((a + in.x) / (2.0 * in.x));
                    break;
                case Op::Tent:
                    r = clamp01(1.0 - std::fabs(a) / in.x);
                    break;

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
                        const Inst &f = b.code[static_cast<std::size_t>(in.c)];
                        const double degree = val[f.a];
                        const double payoff0 = val[f.b];
                        const double w1 = 1.0 - degree;
                        r = payoff0 + degree * (a - payoff0) + w1 * (c - payoff0);
                    }
                    break;
                }
            }
            val[k] = r;
        }
        // Outputs last: every VarIn / PayIn was read from the state before.
        for (std::size_t i = 0; i < b.var_out.size(); ++i)
            if (b.var_out[i] >= 0)
                vars[i] = val[b.var_out[i]];
        if (b.pay_out >= 0)
            payoff = val[b.pay_out];
    }

} // namespace quantModeling::scripting::ir

#endif // QM_SCRIPTING_IR_HPP
