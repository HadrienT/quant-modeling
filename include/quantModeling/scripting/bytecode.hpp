#ifndef QM_SCRIPTING_BYTECODE_HPP
#define QM_SCRIPTING_BYTECODE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

/**
 * @file bytecode.hpp
 * @brief A script compiled to a stack machine, and its interpreter
 *        (blueprint/wp/19-gpu.md §3, lot G2, ADR-G1).
 *
 * The tree of a parsed script (after the front-end passes) is flattened by
 * scripting/compiler.hpp into an array of instructions; `run_event()`
 * interprets one event's range of it on one path. The interpreter touches
 * only raw pointers and fixed-size data, so the same function runs on the
 * CPU (ScriptedProduct) and in a CUDA kernel.
 *
 * It reproduces the tree evaluators operation for operation -- Evaluator<T>
 * (hard) and FuzzyEvaluator<T> (fuzzy) stay the oracles: the 42 scripts of
 * the library price to the same bits either way (tests/testScriptBytecode).
 *
 * Fuzzy `if`s: an interior degree runs both branches and blends them on the
 * variables either branch writes. Scripts have no loop and no recursion, so
 * each `if` executes at most once at a time: its saved state gets a fixed
 * slot range, assigned at compile time, instead of a dynamic frame stack.
 */

namespace quantModeling::scripting
{

    enum class Op : std::uint8_t
    {
        // leaves: push onto the value stack
        Const, ///< x
        Var,   ///< variables[a]
        Spot,  ///< spots[a] of the current event
        Df,    ///< discounts[a] of the current event
        Surv,  ///< survivals[a] of the current event (survivalAbove/Below, WP 26 P2)
        // arithmetic and functions (pop operands, push result)
        Add,
        Sub,
        Mul,
        Div,
        Pow,
        Uplus,
        Neg,
        Min,
        Max,
        Log,
        Exp,
        Sqrt,
        Abs,
        Smooth,
        // hard logic: values 1 / 0 on the value stack
        Eq,
        Ne,
        Gt,
        Ge,
        Lt,
        Le,
        And,
        Or,
        Not,
        // fuzzy logic: degrees on the degree stack
        FCmpEq, ///< a = 1: crisp (discrete, or eps <= 0); x = eps
        FCmpNe,
        FCmpGt,
        FCmpGe,
        FCmpLt,
        FCmpLe,
        FConst, ///< push degree x (a condition folded to always true / false)
        FAnd,
        FOr,
        FNot,
        // statements
        Assign,      ///< variables[a] = pop
        Pays,        ///< payoff += pop / numeraire
        JumpIfFalse, ///< hard if: pop; jump to a when it is 0
        Jump,        ///< to a
        FIfBegin,    ///< fuzzy if a: pop degree; to b (else) when <= 0
        FIfMid,      ///< fuzzy if a: end of then; to b (end) when then-only
        FIfEnd       ///< fuzzy if a: blend
    };

    struct Instr
    {
        Op op = Op::Const;
        std::int32_t a = 0;
        std::int32_t b = 0;
        double x = 0.0;
    };

    /// A fuzzy `if`: its affected variables are aff[first .. first + n), its
    /// saved state if_slots[slot .. slot + 3 + 2n) = degree, payoff before,
    /// payoff after `then`, the variables before, the variables after `then`;
    /// its branch mode is if_mode[mode]. Slots and modes are reused from one
    /// event to the next (two events never run at once): a daily schedule
    /// costs the state of one day, not of the year.
    struct FuzzyIf
    {
        std::int32_t first = 0;
        std::int32_t n = 0;
        std::int32_t slot = 0;
        std::int32_t mode = 0;
    };

    /// Read-only view of a compiled program (host or device memory).
    struct ProgramView
    {
        const Instr *code = nullptr;
        const FuzzyIf *ifs = nullptr;
        const std::int32_t *aff = nullptr;
    };

    /// One path's machine state. Sizes come from the compiled Program.
    template <class T>
    struct Machine
    {
        T *vars = nullptr;      ///< n_vars
        T *stack = nullptr;     ///< max_stack
        T *degrees = nullptr;   ///< max_degrees
        T *if_slots = nullptr;  ///< n_if_slots
        int *if_mode = nullptr; ///< n_if_modes
        T payoff{};
    };

    namespace bytecode_detail
    {
        template <class T>
        QM_HOST_DEVICE double dbl(const T &v)
        {
            return static_cast<double>(v);
        }

        /// FuzzyEvaluator::clamp01.
        template <class T>
        QM_HOST_DEVICE T clamp01(T v)
        {
            const double d = dbl(v);
            if (d <= 0.0)
                return T(0);
            if (d >= 1.0)
                return T(1);
            return v;
        }

        /// FuzzyEvaluator::tent: max(0, 1 − |x|/eps).
        template <class T>
        QM_HOST_DEVICE T tent(const T &x, double eps)
        {
            using std::fabs;
            return clamp01<T>(T(1) - fabs(x) / T(eps));
        }

        /// FuzzyEvaluator::call_spread, the comparison given by `op`.
        template <class T>
        QM_HOST_DEVICE T call_spread(const T &x, double eps, Op op)
        {
            const T up = clamp01<T>((x + T(eps)) / T(2.0 * eps)); // degree of x > 0
            switch (op)
            {
                case Op::FCmpGt:
                case Op::FCmpGe:
                    return up;
                case Op::FCmpLt:
                case Op::FCmpLe:
                    return T(1) - up;
                case Op::FCmpEq:
                    return tent(x, eps);
                case Op::FCmpNe:
                    return T(1) - tent(x, eps);
                default:
                    return up;
            }
        }

        QM_HOST_DEVICE inline bool hard(double x, Op op)
        {
            switch (op)
            {
                case Op::FCmpEq:
                    return x == 0.0;
                case Op::FCmpNe:
                    return x != 0.0;
                case Op::FCmpGt:
                    return x > 0.0;
                case Op::FCmpGe:
                    return x >= 0.0;
                case Op::FCmpLt:
                    return x < 0.0;
                case Op::FCmpLe:
                    return x <= 0.0;
                default:
                    return false;
            }
        }

        enum IfMode : int
        {
            kThenOnly = 0,
            kElseOnly = 1,
            kBlend = 2
        };
    } // namespace bytecode_detail

    /// run_event's recorder when nothing is recorded (compiles to nothing).
    struct NoRecord
    {
        static constexpr bool kRecords = false;
        QM_HOST_DEVICE void push(double) {}
    };

    /**
     * @brief Interpret code[begin, end) -- one event's statements -- on one
     *        path, against that event's spots, discount factors and numeraire.
     *
     * Every number goes through unqualified calls after `using std::…`, as in
     * the tree evaluators, so T = aad::Number records the same tape.
     *
     * `rec` (run_event_recorded) additionally receives, per executed
     * instruction, the operand values its adjoint needs and then its pc.
     */
    template <class T, class Rec>
    QM_HOST_DEVICE void run_event_impl(const ProgramView &p, int begin, int end, Machine<T> &m, const T *spots,
                                       const T *discounts, const T &numeraire, Rec &rec,
                                       const T *survivals = nullptr)
    {
        using namespace bytecode_detail;
        int sp = 0; // value stack
        int dp = 0; // degree stack
        T *S = m.stack;
        T *D = m.degrees;
        int pc = begin;
        // What the adjoint needs (scripting/bytecode_adjoint.hpp): an
        // instruction's operands, then its pc -- read back last-in first-out.
        auto rec2 = [&](const T &a, const T &b)
        {
            if constexpr (Rec::kRecords)
            {
                rec.push(dbl(a));
                rec.push(dbl(b));
            }
        };
        auto rec1 = [&](double a)
        {
            if constexpr (Rec::kRecords)
                rec.push(a);
        };
        while (pc < end)
        {
            const int at = pc;
            const Instr &in = p.code[pc++];
            switch (in.op)
            {
                case Op::Const:
                    S[sp++] = T(in.x);
                    break;
                case Op::Var:
                    S[sp++] = m.vars[in.a];
                    break;
                case Op::Spot:
                    S[sp++] = spots[in.a];
                    break;
                case Op::Df:
                    S[sp++] = discounts[in.a];
                    break;
                case Op::Surv:
                    S[sp++] = survivals[in.a];
                    break;

                case Op::Add:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = a + b;
                    break;
                }
                case Op::Sub:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = a - b;
                    break;
                }
                case Op::Mul:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    rec2(a, b);
                    S[sp++] = a * b;
                    break;
                }
                case Op::Div:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    rec2(a, b);
                    S[sp++] = a / b;
                    break;
                }
                case Op::Pow:
                {
                    using std::pow;
                    const T b = S[--sp];
                    const T a = S[--sp];
                    rec2(a, b);
                    S[sp++] = pow(a, b);
                    break;
                }
                case Op::Uplus:
                    break;
                case Op::Neg:
                {
                    const T a = S[--sp];
                    S[sp++] = -a;
                    break;
                }
                case Op::Min:
                {
                    using std::min;
                    const T b = S[--sp];
                    const T a = S[--sp];
                    rec2(a, b);
                    S[sp++] = min(a, b);
                    break;
                }
                case Op::Max:
                {
                    using std::max;
                    const T b = S[--sp];
                    const T a = S[--sp];
                    rec2(a, b);
                    S[sp++] = max(a, b);
                    break;
                }
                case Op::Log:
                {
                    using std::log;
                    const T a = S[--sp];
                    rec1(dbl(a));
                    S[sp++] = log(a);
                    break;
                }
                case Op::Exp:
                {
                    using std::exp;
                    const T a = S[--sp];
                    S[sp++] = exp(a);
                    rec1(dbl(S[sp - 1]));
                    break;
                }
                case Op::Sqrt:
                {
                    using std::sqrt;
                    const T a = S[--sp];
                    S[sp++] = sqrt(a);
                    rec1(dbl(S[sp - 1]));
                    break;
                }
                case Op::Abs:
                {
                    using std::fabs;
                    const T a = S[--sp];
                    rec1(dbl(a));
                    S[sp++] = fabs(a);
                    break;
                }
                case Op::Smooth: // Evaluator::visit(NodeSmooth)
                {
                    const T h = S[--sp];
                    const T x = S[--sp];
                    rec2(x, h);
                    const double hd = dbl(h);
                    if (hd <= 0.0)
                    {
                        S[sp++] = dbl(x) > 0.0 ? T(1) : T(0);
                        break;
                    }
                    T ramp = (x + h) / (h + h);
                    const double r = dbl(ramp);
                    if (r <= 0.0)
                        S[sp++] = T(0);
                    else if (r >= 1.0)
                        S[sp++] = T(1);
                    else
                        S[sp++] = ramp;
                    break;
                }

                case Op::Eq:
                case Op::Ne:
                case Op::Gt:
                case Op::Ge:
                case Op::Lt:
                case Op::Le:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    bool r = false;
                    switch (in.op)
                    {
                        case Op::Eq:
                            r = a == b;
                            break;
                        case Op::Ne:
                            r = a != b;
                            break;
                        case Op::Gt:
                            r = a > b;
                            break;
                        case Op::Ge:
                            r = a >= b;
                            break;
                        case Op::Lt:
                            r = a < b;
                            break;
                        default:
                            r = a <= b;
                            break;
                    }
                    S[sp++] = r ? T(1) : T(0);
                    break;
                }
                case Op::And:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = (dbl(a) != 0.0) && (dbl(b) != 0.0) ? T(1) : T(0);
                    break;
                }
                case Op::Or:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = (dbl(a) != 0.0) || (dbl(b) != 0.0) ? T(1) : T(0);
                    break;
                }
                case Op::Not:
                {
                    const T a = S[--sp];
                    S[sp++] = (dbl(a) != 0.0) ? T(0) : T(1);
                    break;
                }

                case Op::FCmpEq:
                case Op::FCmpNe:
                case Op::FCmpGt:
                case Op::FCmpGe:
                case Op::FCmpLt:
                case Op::FCmpLe:
                {
                    const T rhs = S[--sp];
                    const T lhs = S[--sp];
                    const T x = lhs - rhs; // condition is x ⋈ 0
                    rec1(dbl(x));
                    if (in.a != 0)
                        D[dp++] = hard(dbl(x), in.op) ? T(1) : T(0);
                    else
                        D[dp++] = call_spread(x, in.x, in.op);
                    break;
                }
                case Op::FConst:
                    D[dp++] = T(in.x);
                    break;
                case Op::FAnd:
                {
                    using std::min;
                    const T b = D[--dp];
                    const T a = D[--dp];
                    rec2(a, b);
                    D[dp++] = min(a, b);
                    break;
                }
                case Op::FOr:
                {
                    using std::max;
                    const T b = D[--dp];
                    const T a = D[--dp];
                    rec2(a, b);
                    D[dp++] = max(a, b);
                    break;
                }
                case Op::FNot:
                {
                    const T a = D[--dp];
                    D[dp++] = T(1) - a;
                    break;
                }

                case Op::Assign:
                    m.vars[in.a] = S[--sp];
                    break;
                case Op::Pays:
                {
                    const T value = S[--sp];
                    rec2(value, numeraire);
                    m.payoff += value / numeraire;
                    break;
                }
                case Op::JumpIfFalse:
                {
                    const T c = S[--sp];
                    if (dbl(c) == 0.0)
                        pc = in.a;
                    break;
                }
                case Op::Jump:
                    pc = in.a;
                    break;

                case Op::FIfBegin: // FuzzyEvaluator::visit(NodeIf), up to the then-branch
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    T *slots = m.if_slots + f.slot;
                    const T degree = D[--dp];
                    const double dt = dbl(degree);
                    if (dt <= 0.0)
                    {
                        m.if_mode[f.mode] = kElseOnly;
                        rec1(kElseOnly);
                        pc = in.b;
                        break;
                    }
                    if (dt >= 1.0)
                    {
                        m.if_mode[f.mode] = kThenOnly;
                        rec1(kThenOnly);
                        break;
                    }
                    m.if_mode[f.mode] = kBlend;
                    rec1(kBlend);
                    slots[0] = degree;
                    slots[1] = m.payoff;
                    for (int i = 0; i < f.n; ++i)
                        slots[3 + i] = m.vars[p.aff[f.first + i]];
                    break;
                }
                case Op::FIfMid:
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    rec1(m.if_mode[f.mode]);
                    if (m.if_mode[f.mode] == kThenOnly)
                    {
                        pc = in.b;
                        break;
                    }
                    T *slots = m.if_slots + f.slot;
                    for (int i = 0; i < f.n; ++i)
                        slots[3 + f.n + i] = m.vars[p.aff[f.first + i]];
                    slots[2] = m.payoff;
                    for (int i = 0; i < f.n; ++i)
                        m.vars[p.aff[f.first + i]] = slots[3 + i];
                    m.payoff = slots[1];
                    break;
                }
                case Op::FIfEnd:
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    if (m.if_mode[f.mode] != kBlend)
                    {
                        rec1(m.if_mode[f.mode]);
                        break;
                    }
                    T *slots = m.if_slots + f.slot;
                    const T &degree = slots[0];
                    const T &payoff0 = slots[1];
                    const T payoff_else = m.payoff;
                    const T w1 = T(1) - degree;
                    if constexpr (Rec::kRecords)
                    {
                        for (int i = 0; i < f.n; ++i)
                            rec1(dbl(slots[3 + f.n + i]));
                        for (int i = 0; i < f.n; ++i)
                            rec1(dbl(m.vars[p.aff[f.first + i]]));
                        rec2(degree, payoff0);
                        rec2(slots[2], payoff_else);
                        rec1(kBlend);
                    }
                    for (int i = 0; i < f.n; ++i)
                    {
                        const int slot = p.aff[f.first + i];
                        m.vars[slot] = degree * slots[3 + f.n + i] + w1 * m.vars[slot];
                    }
                    m.payoff = payoff0 + degree * (slots[2] - payoff0) + w1 * (payoff_else - payoff0);
                    break;
                }
            }
            rec1(static_cast<double>(at));
        }
    }

    template <class T>
    QM_HOST_DEVICE void run_event(const ProgramView &p, int begin, int end, Machine<T> &m, const T *spots,
                                  const T *discounts, const T &numeraire, const T *survivals = nullptr)
    {
        NoRecord none;
        run_event_impl(p, begin, end, m, spots, discounts, numeraire, none, survivals);
    }

    // ── Batched interpreter (blueprint/wp/26-performance.md §5, P1a) ──────

    /// Paths per batch of the CPU's batched interpreter: one decode of an
    /// instruction for kBatch paths.
    inline constexpr int kBatch = 8;
    /// Deepest nesting of ifs (hard or fuzzy) a batched event may have.
    inline constexpr int kMaxBatchDepth = 32;

    /// kBatch paths' machine state, lane-minor: variable a of lane l at
    /// vars[a * kBatch + l], stack slot k at stack[k * kBatch + l], and
    /// likewise the degree stack and the fuzzy ifs' slots (fuzzy scripts).
    struct BatchMachine
    {
        double *vars = nullptr;
        double *stack = nullptr;
        double *degrees = nullptr;  ///< max_degrees * kBatch (fuzzy)
        double *if_slots = nullptr; ///< n_if_slots * kBatch (fuzzy)
        double payoff[kBatch] = {};
    };

    /**
     * @brief code[begin, end) on up to kBatch paths at once (prices): each
     *        instruction is decoded once and applied to every lane, with the
     *        scalar interpreter's operations on each -- the same bits, per
     *        path.
     *
     * A hard `if` is a mask (the compiler emits JumpIfFalse a; then; Jump b;
     * else; b:): the then-block runs for the lanes whose condition holds,
     * the else-block for the others, each skipped when no lane is in it.
     * Every lane computes every value of a block it runs; only `Assign` and
     * `Pays` look at the mask, and they leave a masked lane untouched (no
     * `+ 0`, which would turn a -0 payoff into +0). `lanes` marks the lanes
     * in use; the others must point at valid inputs (a copy of lane 0).
     *
     * A fuzzy `if` (WP 26 P2; FIfBegin; then; FIfMid; else; FIfEnd) splits
     * the active lanes by their degree, as the scalar interpreter splits
     * one path: degree <= 0 runs the else-block only, >= 1 the then-block
     * only, and a degree in between runs both, saving and restoring its
     * state in its own lane of the if's slots and blending at FIfEnd with
     * the scalar formulas.
     */
    inline void run_event_batch(const ProgramView &p, int begin, int end, BatchMachine &m,
                                const double *const *spots, const double *const *discounts, const double *numeraire,
                                unsigned lanes, const double *const *survivals = nullptr)
    {
        constexpr int W = kBatch;
        struct Frame
        {
            unsigned parent, cond;
            int end; // -1 until the then-block's Jump is reached
            bool in_else;
        };
        Frame frames[kMaxBatchDepth];
        int depth = 0;
        // Fuzzy ifs: the lanes that run each branch, and those that blend.
        struct FuzzyFrame
        {
            unsigned parent, then_lanes, else_lanes, blend;
        };
        FuzzyFrame fuzzy[kMaxBatchDepth];
        int fdepth = 0;
        unsigned active = lanes;
        double *S = m.stack;
        int sp = 0;
        double *D = m.degrees;
        int dp = 0;
        int pc = begin;
        auto on = [&](int l)
        { return ((active >> l) & 1u) != 0u; };
        for (;;)
        {
            while (depth > 0 && frames[depth - 1].end == pc)
                active = frames[--depth].parent;
            if (pc >= end)
                break;
            const Instr &in = p.code[pc++];
            double *a = S + (sp - 2) * W; // the two top slots, when they exist
            double *b = S + (sp - 1) * W;
            double *push = S + sp * W;
            switch (in.op)
            {
                case Op::Const:
                    for (int l = 0; l < W; ++l)
                        push[l] = in.x;
                    ++sp;
                    break;
                case Op::Var:
                    for (int l = 0; l < W; ++l)
                        push[l] = m.vars[in.a * W + l];
                    ++sp;
                    break;
                case Op::Spot:
                    for (int l = 0; l < W; ++l)
                        push[l] = spots[l][in.a];
                    ++sp;
                    break;
                case Op::Df:
                    for (int l = 0; l < W; ++l)
                        push[l] = discounts[l][in.a];
                    ++sp;
                    break;
                case Op::Surv:
                    for (int l = 0; l < W; ++l)
                        push[l] = survivals[l][in.a];
                    ++sp;
                    break;
                case Op::Add:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] + b[l];
                    --sp;
                    break;
                case Op::Sub:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] - b[l];
                    --sp;
                    break;
                case Op::Mul:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] * b[l];
                    --sp;
                    break;
                case Op::Div:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] / b[l];
                    --sp;
                    break;
                case Op::Pow:
                    for (int l = 0; l < W; ++l)
                        a[l] = std::pow(a[l], b[l]);
                    --sp;
                    break;
                case Op::Uplus:
                    break;
                case Op::Neg:
                    for (int l = 0; l < W; ++l)
                        b[l] = -b[l];
                    break;
                case Op::Min:
                    for (int l = 0; l < W; ++l)
                        a[l] = std::min(a[l], b[l]);
                    --sp;
                    break;
                case Op::Max:
                    for (int l = 0; l < W; ++l)
                        a[l] = std::max(a[l], b[l]);
                    --sp;
                    break;
                case Op::Log:
                    for (int l = 0; l < W; ++l)
                        b[l] = std::log(b[l]);
                    break;
                case Op::Exp:
                    for (int l = 0; l < W; ++l)
                        b[l] = std::exp(b[l]);
                    break;
                case Op::Sqrt:
                    for (int l = 0; l < W; ++l)
                        b[l] = std::sqrt(b[l]);
                    break;
                case Op::Abs:
                    for (int l = 0; l < W; ++l)
                        b[l] = std::fabs(b[l]);
                    break;
                case Op::Smooth: // as run_event_impl, on double
                    for (int l = 0; l < W; ++l)
                    {
                        const double h = b[l], x = a[l];
                        if (h <= 0.0)
                            a[l] = x > 0.0 ? 1.0 : 0.0;
                        else
                        {
                            const double ramp = (x + h) / (h + h);
                            a[l] = ramp <= 0.0 ? 0.0 : (ramp >= 1.0 ? 1.0 : ramp);
                        }
                    }
                    --sp;
                    break;
                case Op::Eq:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] == b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Ne:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] != b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Gt:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] > b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Ge:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] >= b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Lt:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] < b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Le:
                    for (int l = 0; l < W; ++l)
                        a[l] = a[l] <= b[l] ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::And:
                    for (int l = 0; l < W; ++l)
                        a[l] = (a[l] != 0.0) && (b[l] != 0.0) ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Or:
                    for (int l = 0; l < W; ++l)
                        a[l] = (a[l] != 0.0) || (b[l] != 0.0) ? 1.0 : 0.0;
                    --sp;
                    break;
                case Op::Not:
                    for (int l = 0; l < W; ++l)
                        b[l] = (b[l] != 0.0) ? 0.0 : 1.0;
                    break;

                case Op::Assign:
                    for (int l = 0; l < W; ++l)
                        if (on(l))
                            m.vars[in.a * W + l] = b[l];
                    --sp;
                    break;
                case Op::Pays:
                    for (int l = 0; l < W; ++l)
                        if (on(l))
                            m.payoff[l] += b[l] / numeraire[l];
                    --sp;
                    break;
                case Op::JumpIfFalse: // then-block for the lanes whose condition holds
                {
                    unsigned cond = 0u;
                    for (int l = 0; l < W; ++l)
                        if (b[l] != 0.0)
                            cond |= 1u << l;
                    --sp;
                    cond &= active;
                    frames[depth++] = {active, cond, -1, false};
                    active = cond;
                    if (active == 0u)
                        pc = in.a - 1; // straight to the then-block's Jump
                    break;
                }
                case Op::Jump: // end of the then-block: the else-block's lanes
                {
                    Frame &f = frames[depth - 1];
                    f.in_else = true;
                    f.end = in.a;
                    active = f.parent & ~f.cond;
                    if (active == 0u)
                        pc = in.a;
                    break;
                }
                case Op::FCmpEq:
                case Op::FCmpNe:
                case Op::FCmpGt:
                case Op::FCmpGe:
                case Op::FCmpLt:
                case Op::FCmpLe:
                {
                    double *d = D + dp * W;
                    for (int l = 0; l < W; ++l)
                    {
                        const double x = a[l] - b[l]; // condition is x ⋈ 0
                        d[l] = in.a != 0 ? (bytecode_detail::hard(x, in.op) ? 1.0 : 0.0)
                                         : bytecode_detail::call_spread<double>(x, in.x, in.op);
                    }
                    sp -= 2;
                    ++dp;
                    break;
                }
                case Op::FConst:
                    for (int l = 0; l < W; ++l)
                        D[dp * W + l] = in.x;
                    ++dp;
                    break;
                case Op::FAnd:
                {
                    double *da = D + (dp - 2) * W;
                    const double *db = D + (dp - 1) * W;
                    for (int l = 0; l < W; ++l)
                        da[l] = std::min(da[l], db[l]);
                    --dp;
                    break;
                }
                case Op::FOr:
                {
                    double *da = D + (dp - 2) * W;
                    const double *db = D + (dp - 1) * W;
                    for (int l = 0; l < W; ++l)
                        da[l] = std::max(da[l], db[l]);
                    --dp;
                    break;
                }
                case Op::FNot:
                {
                    double *db = D + (dp - 1) * W;
                    for (int l = 0; l < W; ++l)
                        db[l] = 1.0 - db[l];
                    break;
                }
                case Op::FIfBegin: // run_event_impl's FIfBegin, per lane
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    double *slots = m.if_slots + f.slot * W;
                    const double *deg = D + (--dp) * W;
                    unsigned then_only = 0u, else_only = 0u, blend = 0u;
                    for (int l = 0; l < W; ++l)
                    {
                        if (!on(l))
                            continue;
                        const double d = deg[l];
                        if (d <= 0.0)
                            else_only |= 1u << l;
                        else if (d >= 1.0)
                            then_only |= 1u << l;
                        else
                        {
                            blend |= 1u << l;
                            slots[l] = d;
                            slots[W + l] = m.payoff[l];
                            for (int i = 0; i < f.n; ++i)
                                slots[(3 + i) * W + l] = m.vars[p.aff[f.first + i] * W + l];
                        }
                    }
                    fuzzy[fdepth++] = {active, then_only | blend, else_only | blend, blend};
                    active = then_only | blend;
                    if (active == 0u) // no lane in the then-block: straight to the else-block
                    {
                        active = else_only;
                        pc = in.b;
                    }
                    break;
                }
                case Op::FIfMid: // end of the then-block: the blending lanes start over
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    double *slots = m.if_slots + f.slot * W;
                    FuzzyFrame &fr = fuzzy[fdepth - 1];
                    for (int l = 0; l < W; ++l)
                    {
                        if (((fr.blend >> l) & 1u) == 0u)
                            continue;
                        for (int i = 0; i < f.n; ++i)
                            slots[(3 + f.n + i) * W + l] = m.vars[p.aff[f.first + i] * W + l];
                        slots[2 * W + l] = m.payoff[l];
                        for (int i = 0; i < f.n; ++i)
                            m.vars[p.aff[f.first + i] * W + l] = slots[(3 + i) * W + l];
                        m.payoff[l] = slots[W + l];
                    }
                    active = fr.else_lanes;
                    if (active == 0u) // no lane in the else-block: past FIfEnd
                    {
                        active = fr.parent;
                        --fdepth;
                        pc = in.b;
                    }
                    break;
                }
                case Op::FIfEnd: // the blend, as run_event_impl writes it
                {
                    const FuzzyIf &f = p.ifs[in.a];
                    const double *slots = m.if_slots + f.slot * W;
                    const FuzzyFrame fr = fuzzy[--fdepth];
                    for (int l = 0; l < W; ++l)
                    {
                        if (((fr.blend >> l) & 1u) == 0u)
                            continue;
                        const double degree = slots[l];
                        const double payoff0 = slots[W + l];
                        const double payoff_else = m.payoff[l];
                        const double w1 = 1.0 - degree;
                        for (int i = 0; i < f.n; ++i)
                        {
                            double &v = m.vars[p.aff[f.first + i] * W + l];
                            v = degree * slots[(3 + f.n + i) * W + l] + w1 * v;
                        }
                        m.payoff[l] = payoff0 + degree * (slots[2 * W + l] - payoff0) + w1 * (payoff_else - payoff0);
                    }
                    active = fr.parent;
                    break;
                }
            }
        }
    }

    /// A compiled script (host memory).
    struct Program
    {
        std::vector<Instr> code;
        std::vector<FuzzyIf> ifs;
        std::vector<std::int32_t> aff;
        /// Event e runs code[event_begin[e], event_begin[e + 1]).
        std::vector<std::int32_t> event_begin;
        /// Highest spot index / discount slot / survival slot event e reads
        /// (-1: none).
        std::vector<std::int32_t> event_max_spot, event_max_df, event_max_surv;
        int n_vars = 0;
        int max_stack = 0;
        int max_degrees = 0;
        int n_if_slots = 0; ///< the most any one event needs
        int n_if_modes = 0;
        bool fuzzy = false;
        /// The script reads survivalAbove/Below: a path that does not carry
        /// the model's survival probabilities (the GPU kernels, the per-path
        /// adjoint) must refuse it.
        bool has_survival = false;

        ProgramView view() const { return {code.data(), ifs.data(), aff.data()}; }

        /// Whether run_event_batch can run every event (P1a; fuzzy ifs since
        /// WP 26 P2): ifs nested at most kMaxBatchDepth deep, in the
        /// compiler's layout.
        bool batchable() const
        {
            if (code.empty() || event_begin.size() < 2)
                return false;
            for (std::size_t e = 0; e + 1 < event_begin.size(); ++e)
            {
                const int begin = event_begin[e], end = event_begin[e + 1];
                std::vector<int> ends; // open hard ifs' end pcs
                int fuzzy_depth = 0;   // open fuzzy ifs
                for (int pc = begin; pc < end; ++pc)
                {
                    while (!ends.empty() && ends.back() == pc)
                        ends.pop_back();
                    const Instr &in = code[static_cast<std::size_t>(pc)];
                    if (in.op == Op::FIfBegin || in.op == Op::FIfMid)
                    {
                        // FIfBegin b: the else-block; FIfMid b: past FIfEnd.
                        if (in.b <= pc || in.b > end)
                            return false;
                        if (in.op == Op::FIfBegin && ++fuzzy_depth > kMaxBatchDepth)
                            return false;
                    }
                    if (in.op == Op::FIfEnd && --fuzzy_depth < 0)
                        return false;
                    if (in.op == Op::JumpIfFalse)
                    {
                        // JumpIfFalse a; then; Jump b at a - 1; else; b:
                        if (in.a <= pc + 1 || in.a > end)
                            return false;
                        const Instr &j = code[static_cast<std::size_t>(in.a - 1)];
                        if (j.op != Op::Jump || j.a < in.a || j.a > end)
                            return false;
                        ends.push_back(j.a);
                        if (static_cast<int>(ends.size()) > kMaxBatchDepth)
                            return false;
                    }
                }
                if (fuzzy_depth != 0)
                    return false;
            }
            return true;
        }
        std::size_t n_events() const { return event_begin.empty() ? 0 : event_begin.size() - 1; }
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_BYTECODE_HPP
