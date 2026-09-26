#ifndef QM_SCRIPTING_BYTECODE_HPP
#define QM_SCRIPTING_BYTECODE_HPP

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
    /// payoff after `then`, the variables before, the variables after `then`.
    struct FuzzyIf
    {
        std::int32_t first = 0;
        std::int32_t n = 0;
        std::int32_t slot = 0;
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
        int *if_mode = nullptr; ///< one per fuzzy if
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

    /**
     * @brief Interpret code[begin, end) -- one event's statements -- on one
     *        path, against that event's spots, discount factors and numeraire.
     *
     * Every number goes through unqualified calls after `using std::…`, as in
     * the tree evaluators, so T = aad::Number records the same tape.
     */
    template <class T>
    QM_HOST_DEVICE void run_event(const ProgramView &p, int begin, int end, Machine<T> &m, const T *spots,
                                  const T *discounts, const T &numeraire)
    {
        using namespace bytecode_detail;
        int sp = 0; // value stack
        int dp = 0; // degree stack
        T *S = m.stack;
        T *D = m.degrees;
        int pc = begin;
        while (pc < end)
        {
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
                    S[sp++] = a * b;
                    break;
                }
                case Op::Div:
                {
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = a / b;
                    break;
                }
                case Op::Pow:
                {
                    using std::pow;
                    const T b = S[--sp];
                    const T a = S[--sp];
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
                    S[sp++] = min(a, b);
                    break;
                }
                case Op::Max:
                {
                    using std::max;
                    const T b = S[--sp];
                    const T a = S[--sp];
                    S[sp++] = max(a, b);
                    break;
                }
                case Op::Log:
                {
                    using std::log;
                    const T a = S[--sp];
                    S[sp++] = log(a);
                    break;
                }
                case Op::Exp:
                {
                    using std::exp;
                    const T a = S[--sp];
                    S[sp++] = exp(a);
                    break;
                }
                case Op::Sqrt:
                {
                    using std::sqrt;
                    const T a = S[--sp];
                    S[sp++] = sqrt(a);
                    break;
                }
                case Op::Abs:
                {
                    using std::fabs;
                    const T a = S[--sp];
                    S[sp++] = fabs(a);
                    break;
                }
                case Op::Smooth: // Evaluator::visit(NodeSmooth)
                {
                    const T h = S[--sp];
                    const T x = S[--sp];
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
                    D[dp++] = min(a, b);
                    break;
                }
                case Op::FOr:
                {
                    using std::max;
                    const T b = D[--dp];
                    const T a = D[--dp];
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
                        m.if_mode[in.a] = kElseOnly;
                        pc = in.b;
                        break;
                    }
                    if (dt >= 1.0)
                    {
                        m.if_mode[in.a] = kThenOnly;
                        break;
                    }
                    m.if_mode[in.a] = kBlend;
                    slots[0] = degree;
                    slots[1] = m.payoff;
                    for (int i = 0; i < f.n; ++i)
                        slots[3 + i] = m.vars[p.aff[f.first + i]];
                    break;
                }
                case Op::FIfMid:
                {
                    if (m.if_mode[in.a] == kThenOnly)
                    {
                        pc = in.b;
                        break;
                    }
                    const FuzzyIf &f = p.ifs[in.a];
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
                    if (m.if_mode[in.a] != kBlend)
                        break;
                    const FuzzyIf &f = p.ifs[in.a];
                    T *slots = m.if_slots + f.slot;
                    const T &degree = slots[0];
                    const T &payoff0 = slots[1];
                    const T payoff_else = m.payoff;
                    const T w1 = T(1) - degree;
                    for (int i = 0; i < f.n; ++i)
                    {
                        const int slot = p.aff[f.first + i];
                        m.vars[slot] = degree * slots[3 + f.n + i] + w1 * m.vars[slot];
                    }
                    m.payoff = payoff0 + degree * (slots[2] - payoff0) + w1 * (payoff_else - payoff0);
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
        /// Highest spot index / discount slot event e reads (-1: none).
        std::vector<std::int32_t> event_max_spot, event_max_df;
        int n_vars = 0;
        int max_stack = 0;
        int max_degrees = 0;
        int n_if_slots = 0;
        bool fuzzy = false;

        ProgramView view() const { return {code.data(), ifs.data(), aff.data()}; }
        std::size_t n_events() const { return event_begin.empty() ? 0 : event_begin.size() - 1; }
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_BYTECODE_HPP
