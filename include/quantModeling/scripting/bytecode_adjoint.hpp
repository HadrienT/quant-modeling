#ifndef QM_SCRIPTING_BYTECODE_ADJOINT_HPP
#define QM_SCRIPTING_BYTECODE_ADJOINT_HPP

#include <cmath>
#include <cstdint>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/scripting/bytecode.hpp"

/**
 * @file bytecode_adjoint.hpp
 * @brief The adjoint of the script interpreter, instruction by instruction
 *        (blueprint/wp/19-gpu.md §6.2, lot G3, ADR-G4).
 *
 * The GPU has no tape (ADR-G4). What it has instead, per path, is a *trail*:
 * run_event_recorded() runs an event forward exactly as run_event() does and
 * pushes, for each executed instruction, the operand values its adjoint
 * needs and then its pc; reverse_event() pops them back and applies each
 * instruction's adjoint -- written once here, mechanically, one case per
 * opcode. Scripts have no loops, so an event executes each of its
 * instructions at most once: the trail of a path has a size known at compile
 * time (trail_bound), which is what lets the GPU size its memory in advance.
 *
 * The local derivatives, and the side taken at a kink, are those of the CPU
 * tape (aad/expression.hpp): max(l, r) sends the adjoint to l when l > r,
 * min when l < r, fabs takes +1 at 0, a clamp passes nothing at its bounds,
 * a hard comparison and a hard `if` pass nothing to their operands. The CPU
 * adjoint (ScriptedProduct<aad::Number>) is the oracle: same draws, same
 * risks up to rounding (tests/testScriptAdjoint.cpp).
 *
 * Host and device: plain doubles, raw pointers, no allocation.
 */

namespace quantModeling::scripting
{

    /// A path's forward record: element k at base[k * stride] (stride = the
    /// number of GPU threads sharing the buffer, for coalesced access; 1 on
    /// the host). Last in, first out.
    struct Trail
    {
        double *base = nullptr;
        long stride = 1;
        long top = 0;

        static constexpr bool kRecords = true;
        QM_HOST_DEVICE void push(double x) { base[top++ * stride] = x; }
        QM_HOST_DEVICE double pop() { return base[--top * stride]; }
    };

    /// Marks the start of an event on the trail (a pc is never negative).
    constexpr double kEventMark = -1.0;

    /// Entries an event can push at most (its sentinel included).
    inline long trail_bound(const Program &p, std::size_t event)
    {
        long n = 1;
        for (int pc = p.event_begin[event]; pc < p.event_begin[event + 1]; ++pc)
        {
            const Instr &in = p.code[static_cast<std::size_t>(pc)];
            n += 1; // the pc
            switch (in.op)
            {
                case Op::Mul:
                case Op::Div:
                case Op::Pow:
                case Op::Min:
                case Op::Max:
                case Op::Smooth:
                case Op::FAnd:
                case Op::FOr:
                case Op::Pays:
                    n += 2;
                    break;
                case Op::Log:
                case Op::Exp:
                case Op::Sqrt:
                case Op::Abs:
                case Op::FCmpEq:
                case Op::FCmpNe:
                case Op::FCmpGt:
                case Op::FCmpGe:
                case Op::FCmpLt:
                case Op::FCmpLe:
                case Op::FIfBegin:
                case Op::FIfMid:
                    n += 1;
                    break;
                case Op::FIfEnd:
                    n += 5 + 2 * p.ifs[static_cast<std::size_t>(in.a)].n;
                    break;
                default:
                    break;
            }
        }
        return n;
    }

    /// run_event, recording the event on the trail after a kEventMark.
    QM_HOST_DEVICE inline void run_event_recorded(const ProgramView &p, int begin, int end, Machine<double> &m,
                                                  const double *spots, const double *discounts, double numeraire,
                                                  Trail &trail)
    {
        trail.push(kEventMark);
        run_event_impl(p, begin, end, m, spots, discounts, numeraire, trail);
    }

    /// Adjoints of one path's machine: the variables, the value and degree
    /// stacks, the fuzzy-if slots and the payoff. Zero at the end of the
    /// path, except `payoff` (the seed, 1).
    struct AdjointMachine
    {
        double *vars = nullptr;
        double *stack = nullptr;
        double *degrees = nullptr;
        double *if_slots = nullptr;
        double payoff = 0.0;
    };

    /**
     * @brief Reverse the last event recorded on `trail`: add the adjoints of
     *        its spots, discount factors and numeraire into spots_adj,
     *        discounts_adj and numeraire_adj, and update `a` to the adjoints
     *        before the event.
     */
    QM_HOST_DEVICE inline void reverse_event(const ProgramView &p, AdjointMachine &a, double *spots_adj,
                                             double *discounts_adj, double &numeraire_adj, Trail &trail)
    {
        using namespace bytecode_detail;
        double *S = a.stack;
        double *D = a.degrees;
        int sp = 0, dp = 0; // an event starts and ends with empty stacks
        for (;;)
        {
            const double tag = trail.pop();
            if (tag < 0.0)
                return; // kEventMark
            const Instr &in = p.code[static_cast<int>(tag)];
            switch (in.op)
            {
                case Op::Const:
                    --sp;
                    break;
                case Op::Var:
                    a.vars[in.a] += S[--sp];
                    break;
                case Op::Spot:
                    spots_adj[in.a] += S[--sp];
                    break;
                case Op::Df:
                    discounts_adj[in.a] += S[--sp];
                    break;

                case Op::Add:
                {
                    const double g = S[--sp];
                    S[sp++] = g;
                    S[sp++] = g;
                    break;
                }
                case Op::Sub:
                {
                    const double g = S[--sp];
                    S[sp++] = g;
                    S[sp++] = -g;
                    break;
                }
                case Op::Mul:
                {
                    const double b = trail.pop(), x = trail.pop();
                    const double g = S[--sp];
                    S[sp++] = g * b;
                    S[sp++] = g * x;
                    break;
                }
                case Op::Div:
                {
                    const double b = trail.pop(), x = trail.pop();
                    const double g = S[--sp];
                    const double v = x / b;
                    S[sp++] = g * (1.0 / b);
                    S[sp++] = g * (-v / b);
                    break;
                }
                case Op::Pow:
                {
                    const double b = trail.pop(), x = trail.pop();
                    const double g = S[--sp];
                    const double v = std::pow(x, b);
                    S[sp++] = g * (b * v / x);
                    S[sp++] = g * (v * std::log(x));
                    break;
                }
                case Op::Uplus:
                    break;
                case Op::Neg:
                    S[sp - 1] = -S[sp - 1];
                    break;
                case Op::Min:
                case Op::Max:
                {
                    const double b = trail.pop(), x = trail.pop();
                    const double g = S[--sp];
                    const bool left = in.op == Op::Min ? x < b : x > b;
                    S[sp++] = left ? g : 0.0;
                    S[sp++] = left ? 0.0 : g;
                    break;
                }
                case Op::Log:
                {
                    const double x = trail.pop();
                    S[sp - 1] *= 1.0 / x;
                    break;
                }
                case Op::Exp:
                {
                    const double v = trail.pop();
                    S[sp - 1] *= v;
                    break;
                }
                case Op::Sqrt:
                {
                    const double v = trail.pop();
                    S[sp - 1] *= 0.5 / v;
                    break;
                }
                case Op::Abs:
                {
                    const double x = trail.pop();
                    S[sp - 1] *= x >= 0.0 ? 1.0 : -1.0;
                    break;
                }
                case Op::Smooth:
                {
                    const double h = trail.pop(), x = trail.pop();
                    const double g = S[--sp];
                    double gx = 0.0, gh = 0.0;
                    if (h > 0.0)
                    {
                        // ramp = (x + h) / (h + h), passed through strictly inside (0, 1)
                        const double den = h + h;
                        const double ramp = (x + h) / den;
                        if (ramp > 0.0 && ramp < 1.0)
                        {
                            const double num_adj = g * (1.0 / den);
                            const double den_adj = g * (-ramp / den);
                            gx = num_adj;
                            gh = num_adj + den_adj + den_adj;
                        }
                    }
                    S[sp++] = gx;
                    S[sp++] = gh;
                    break;
                }

                case Op::Eq:
                case Op::Ne:
                case Op::Gt:
                case Op::Ge:
                case Op::Lt:
                case Op::Le:
                case Op::And:
                case Op::Or:
                    --sp;
                    S[sp++] = 0.0;
                    S[sp++] = 0.0;
                    break;
                case Op::Not:
                    S[sp - 1] = 0.0;
                    break;

                case Op::FCmpEq:
                case Op::FCmpNe:
                case Op::FCmpGt:
                case Op::FCmpGe:
                case Op::FCmpLt:
                case Op::FCmpLe:
                {
                    const double x = trail.pop();
                    const double gd = D[--dp];
                    double gx = 0.0;
                    if (in.a == 0)
                    {
                        const double eps = in.x;
                        if (in.op == Op::FCmpEq || in.op == Op::FCmpNe)
                        {
                            // tent = clamp01(1 - |x| / eps)
                            const double inner = 1.0 - std::fabs(x) / eps;
                            if (inner > 0.0 && inner < 1.0)
                                gx = -(x >= 0.0 ? 1.0 : -1.0) / eps;
                            if (in.op == Op::FCmpNe)
                                gx = -gx;
                        }
                        else
                        {
                            // up = clamp01((x + eps) / (2 eps))
                            const double inner = (x + eps) / (2.0 * eps);
                            if (inner > 0.0 && inner < 1.0)
                                gx = 1.0 / (2.0 * eps);
                            if (in.op == Op::FCmpLt || in.op == Op::FCmpLe)
                                gx = -gx;
                        }
                        gx *= gd;
                    }
                    S[sp++] = gx;  // lhs
                    S[sp++] = -gx; // rhs
                    break;
                }
                case Op::FConst:
                    --dp;
                    break;
                case Op::FAnd:
                case Op::FOr:
                {
                    const double b = trail.pop(), x = trail.pop();
                    const double g = D[--dp];
                    const bool left = in.op == Op::FAnd ? x < b : x > b;
                    D[dp++] = left ? g : 0.0;
                    D[dp++] = left ? 0.0 : g;
                    break;
                }
                case Op::FNot:
                    D[dp - 1] = -D[dp - 1];
                    break;

                case Op::Assign:
                    S[sp++] = a.vars[in.a];
                    a.vars[in.a] = 0.0;
                    break;
                case Op::Pays:
                {
                    const double num = trail.pop(), value = trail.pop();
                    const double g = a.payoff;
                    const double v = value / num;
                    S[sp++] = g * (1.0 / num);
                    numeraire_adj += g * (-v / num);
                    break;
                }
                case Op::JumpIfFalse:
                    S[sp++] = 0.0;
                    break;
                case Op::Jump:
                    break;

                case Op::FIfBegin:
                {
                    const int mode = static_cast<int>(trail.pop());
                    const FuzzyIf &f = p.ifs[in.a];
                    double *A = a.if_slots + f.slot;
                    if (mode != kBlend)
                    {
                        D[dp++] = 0.0;
                        break;
                    }
                    for (int i = 0; i < f.n; ++i)
                    {
                        a.vars[p.aff[f.first + i]] += A[3 + i];
                        A[3 + i] = 0.0;
                    }
                    a.payoff += A[1];
                    A[1] = 0.0;
                    D[dp++] = A[0];
                    A[0] = 0.0;
                    break;
                }
                case Op::FIfMid:
                {
                    const int mode = static_cast<int>(trail.pop());
                    if (mode != kBlend)
                        break; // then-only: a jump
                    const FuzzyIf &f = p.ifs[in.a];
                    double *A = a.if_slots + f.slot;
                    // restore of the variables and payoff, reversed
                    for (int i = 0; i < f.n; ++i)
                    {
                        const int v = p.aff[f.first + i];
                        A[3 + i] += a.vars[v];
                        a.vars[v] = 0.0;
                    }
                    A[1] += a.payoff;
                    a.payoff = 0.0;
                    // save of the then-results, reversed
                    a.payoff += A[2];
                    A[2] = 0.0;
                    for (int i = 0; i < f.n; ++i)
                    {
                        a.vars[p.aff[f.first + i]] += A[3 + f.n + i];
                        A[3 + f.n + i] = 0.0;
                    }
                    break;
                }
                case Op::FIfEnd:
                {
                    const int mode = static_cast<int>(trail.pop());
                    if (mode != kBlend)
                        break;
                    const FuzzyIf &f = p.ifs[in.a];
                    double *A = a.if_slots + f.slot;
                    const double pe = trail.pop(), pt = trail.pop();
                    const double p0 = trail.pop(), degree = trail.pop();
                    const double w1 = 1.0 - degree;
                    // vars[v] = degree * then + w1 * else: the else values
                    // come off the trail first, then the then values.
                    double g_degree = 0.0;
                    for (int i = f.n - 1; i >= 0; --i)
                        g_degree -= a.vars[p.aff[f.first + i]] * trail.pop();
                    for (int i = f.n - 1; i >= 0; --i)
                    {
                        const double gv = a.vars[p.aff[f.first + i]];
                        g_degree += gv * trail.pop();
                        A[3 + f.n + i] += gv * degree;
                    }
                    for (int i = 0; i < f.n; ++i)
                        a.vars[p.aff[f.first + i]] *= w1;
                    // payoff = p0 + degree (pt - p0) + w1 (pe - p0)
                    const double gp = a.payoff;
                    g_degree += gp * (pt - p0) - gp * (pe - p0);
                    A[2] += gp * degree;
                    A[1] += gp - gp * degree - gp * w1;
                    a.payoff = gp * w1;
                    A[0] += g_degree;
                    break;
                }
            }
        }
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_BYTECODE_ADJOINT_HPP
