#include "quantModeling/scripting/cuda_codegen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace quantModeling::scripting
{

    namespace
    {
        /// A double as C source, exact to the bit: a hexadecimal floating
        /// literal, or its bit pattern when it is not finite.
        std::string lit(double x)
        {
            char buf[64];
            if (std::isfinite(x))
                std::snprintf(buf, sizeof buf, "%a", x);
            else
            {
                std::uint64_t bits = 0;
                std::memcpy(&bits, &x, sizeof bits);
                std::snprintf(buf, sizeof buf, "__longlong_as_double(0x%016llxLL)",
                              static_cast<unsigned long long>(bits));
            }
            return buf;
        }

        std::string s(int i)
        {
            return "s" + std::to_string(i);
        }
        std::string d(int i)
        {
            return "d" + std::to_string(i);
        }
        std::string q(int i)
        {
            return "q" + std::to_string(i);
        }
        std::string mode(int i)
        {
            return "m" + std::to_string(i);
        }
        std::string var(int i)
        {
            return "v[" + std::to_string(i) + "]";
        }

        /// Values each opcode pops / pushes on the value stack (S) and on the
        /// degree stack (D) -- run_event_impl's.
        struct Effect
        {
            int s_pop, s_push, d_pop, d_push;
        };

        Effect effect(Op op)
        {
            switch (op)
            {
                case Op::Const:
                case Op::Var:
                case Op::Spot:
                case Op::Df:
                    return {0, 1, 0, 0};
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Pow:
                case Op::Min:
                case Op::Max:
                case Op::Smooth:
                case Op::Eq:
                case Op::Ne:
                case Op::Gt:
                case Op::Ge:
                case Op::Lt:
                case Op::Le:
                case Op::And:
                case Op::Or:
                    return {2, 1, 0, 0};
                case Op::Uplus:
                case Op::Neg:
                case Op::Log:
                case Op::Exp:
                case Op::Sqrt:
                case Op::Abs:
                case Op::Not:
                    return {1, 1, 0, 0};
                case Op::FCmpEq:
                case Op::FCmpNe:
                case Op::FCmpGt:
                case Op::FCmpGe:
                case Op::FCmpLt:
                case Op::FCmpLe:
                    return {2, 0, 0, 1};
                case Op::FConst:
                    return {0, 0, 0, 1};
                case Op::FAnd:
                case Op::FOr:
                    return {0, 0, 2, 1};
                case Op::FNot:
                    return {0, 0, 1, 1};
                case Op::Assign:
                case Op::Pays:
                case Op::JumpIfFalse:
                    return {1, 0, 0, 0};
                case Op::Jump:
                case Op::FIfMid:
                case Op::FIfEnd:
                    return {0, 0, 0, 0};
                case Op::FIfBegin:
                    return {0, 0, 1, 0};
            }
            return {0, 0, 0, 0};
        }

        constexpr int kUnreached = -1;

        /// The stack depths before each instruction of code[begin, end),
        /// found by following every edge; false if two paths disagree or a
        /// jump leaves the event.
        bool depths(const Program &p, int begin, int end, std::vector<int> &sd, std::vector<int> &dd,
                    std::vector<char> &target, std::string &why)
        {
            const auto at = [&](int pc)
            { return static_cast<std::size_t>(pc - begin); };
            const std::size_t n = static_cast<std::size_t>(end - begin) + 1; // + the event's end
            sd.assign(n, kUnreached);
            dd.assign(n, kUnreached);
            target.assign(n, 0);
            std::vector<int> work{begin};
            sd[0] = 0;
            dd[0] = 0;
            auto reach = [&](int to, int s_depth, int d_depth, bool jump) -> bool
            {
                if (to < begin || to > end)
                {
                    why = "a jump leaves its event";
                    return false;
                }
                if (jump)
                    target[at(to)] = 1;
                if (sd[at(to)] == kUnreached)
                {
                    sd[at(to)] = s_depth;
                    dd[at(to)] = d_depth;
                    if (to < end)
                        work.push_back(to);
                    return true;
                }
                if (sd[at(to)] != s_depth || dd[at(to)] != d_depth)
                {
                    why = "the stack depth differs between two paths";
                    return false;
                }
                return true;
            };
            while (!work.empty())
            {
                const int pc = work.back();
                work.pop_back();
                const Instr &in = p.code[static_cast<std::size_t>(pc)];
                const Effect e = effect(in.op);
                const int s0 = sd[at(pc)], d0 = dd[at(pc)];
                if (s0 < e.s_pop || d0 < e.d_pop)
                {
                    why = "the stack underflows";
                    return false;
                }
                const int s1 = s0 - e.s_pop + e.s_push, d1 = d0 - e.d_pop + e.d_push;
                if (s1 > p.max_stack || d1 > p.max_degrees)
                {
                    why = "the stack overflows";
                    return false;
                }
                switch (in.op)
                {
                    case Op::Jump:
                        if (!reach(in.a, s1, d1, true))
                            return false;
                        break;
                    case Op::JumpIfFalse:
                        if (!reach(in.a, s1, d1, true) || !reach(pc + 1, s1, d1, false))
                            return false;
                        break;
                    case Op::FIfBegin:
                    case Op::FIfMid:
                        if (!reach(in.b, s1, d1, true) || !reach(pc + 1, s1, d1, false))
                            return false;
                        break;
                    default:
                        if (!reach(pc + 1, s1, d1, false))
                            return false;
                }
            }
            if (sd[at(end)] != kUnreached && (sd[at(end)] != 0 || dd[at(end)] != 0))
            {
                why = "an event ends with values left on its stack";
                return false;
            }
            return true;
        }

        /// The comparison of `op` on x ⋈ 0, crisp (bytecode_detail::hard).
        const char *hard_cmp(Op op)
        {
            switch (op)
            {
                case Op::FCmpEq:
                    return "==";
                case Op::FCmpNe:
                    return "!=";
                case Op::FCmpGt:
                    return ">";
                case Op::FCmpGe:
                    return ">=";
                case Op::FCmpLt:
                    return "<";
                default:
                    return "<=";
            }
        }

        const char *crisp_cmp(Op op)
        {
            switch (op)
            {
                case Op::Eq:
                    return "==";
                case Op::Ne:
                    return "!=";
                case Op::Gt:
                    return ">";
                case Op::Ge:
                    return ">=";
                case Op::Lt:
                    return "<";
                default:
                    return "<=";
            }
        }

        /// The helpers of bytecode_detail, on double, written as there.
        constexpr const char *kPrelude = R"(// Generated from a script's bytecode (quantModeling, scripting/cuda_codegen.cpp).
// Compiled with --fmad=false, as the interpreter; the stack arithmetic goes
// through __dadd_rn / __dsub_rn / __dmul_rn, never contracted in any case.

static __device__ __forceinline__ double qm_clamp01(double v)
{
    const double d = v;
    if (d <= 0.0)
        return 0.0;
    if (d >= 1.0)
        return 1.0;
    return v;
}
static __device__ __forceinline__ double qm_tent(double x, double eps)
{
    return qm_clamp01(1.0 - fabs(x) / eps);
}
static __device__ __forceinline__ double qm_up(double x, double eps, double two_eps)
{
    return qm_clamp01((x + eps) / two_eps);
}

)";
    } // namespace

    CudaSource cuda_event_source(const Program &p)
    {
        CudaSource out;
        if (p.code.empty() || p.event_begin.size() < 2)
        {
            out.why = "the script is not compiled to bytecode";
            return out;
        }
        std::ostringstream src;
        src << kPrelude;
        src << "extern \"C\" __device__ void " << kCudaEventFunction
            << "(int e, double *__restrict__ v, const double *__restrict__ spot, const double *__restrict__ df,\n"
            << "                                    double num, double *__restrict__ payoff)\n{\n"
            << "    switch (e)\n    {\n";

        std::vector<std::string> bodies;     // distinct event bodies
        std::vector<std::vector<int>> cases; // the events of each
        for (std::size_t ev = 0; ev + 1 < p.event_begin.size(); ++ev)
        {
            const int begin = p.event_begin[ev], end = p.event_begin[ev + 1];
            std::vector<int> sd, dd;
            std::vector<char> target;
            if (!depths(p, begin, end, sd, dd, target, out.why))
            {
                out.why = "event " + std::to_string(ev) + ": " + out.why;
                return out;
            }
            std::ostringstream o; // this event's body; labels "@<offset>", made unique below
            // Locals: the stack slots, the degree slots, the fuzzy ifs'
            // saved state and branch modes (reused from one event to the
            // next, as in the interpreter).
            auto decl = [&](const char *type, const char *prefix, int count, const char *init)
            {
                if (count <= 0)
                    return;
                o << "        " << type << " ";
                for (int i = 0; i < count; ++i)
                    o << (i ? ", " : "") << prefix << i << " = " << init;
                o << ";\n";
            };
            decl("double", "s", p.max_stack, "0.0");
            decl("double", "d", p.max_degrees, "0.0");
            decl("double", "q", p.n_if_slots, "0.0");
            decl("int", "m", p.n_if_modes, "0");
            o << "        double P = *payoff;\n";

            for (int pc = begin; pc < end; ++pc)
            {
                const std::size_t k = static_cast<std::size_t>(pc - begin);
                if (target[k])
                    o << "    @" << pc - begin << ":;\n";
                if (sd[k] == kUnreached)
                    continue;
                const Instr &in = p.code[static_cast<std::size_t>(pc)];
                const int t = sd[k];  // value stack depth before
                const int dt = dd[k]; // degree stack depth before
                const std::string a = t >= 2 ? s(t - 2) : "", b = t >= 1 ? s(t - 1) : "";
                o << "        ";
                switch (in.op)
                {
                    case Op::Const:
                        o << s(t) << " = " << lit(in.x) << ";";
                        break;
                    case Op::Var:
                        o << s(t) << " = " << var(in.a) << ";";
                        break;
                    case Op::Spot:
                        o << s(t) << " = spot[" << in.a << "];";
                        break;
                    case Op::Df:
                        o << s(t) << " = df[" << in.a << "];";
                        break;
                    case Op::Add:
                        o << a << " = __dadd_rn(" << a << ", " << b << ");";
                        break;
                    case Op::Sub:
                        o << a << " = __dsub_rn(" << a << ", " << b << ");";
                        break;
                    case Op::Mul:
                        o << a << " = __dmul_rn(" << a << ", " << b << ");";
                        break;
                    case Op::Div:
                        o << a << " = " << a << " / " << b << ";";
                        break;
                    case Op::Pow:
                        o << a << " = pow(" << a << ", " << b << ");";
                        break;
                    case Op::Uplus:
                        o << "// +x";
                        break;
                    case Op::Neg:
                        o << b << " = -" << b << ";";
                        break;
                    case Op::Min: // std::min(a, b): (b < a) ? b : a
                        o << a << " = (" << b << " < " << a << ") ? " << b << " : " << a << ";";
                        break;
                    case Op::Max: // std::max(a, b): (a < b) ? b : a
                        o << a << " = (" << a << " < " << b << ") ? " << b << " : " << a << ";";
                        break;
                    case Op::Log:
                        o << b << " = log(" << b << ");";
                        break;
                    case Op::Exp:
                        o << b << " = exp(" << b << ");";
                        break;
                    case Op::Sqrt:
                        o << b << " = sqrt(" << b << ");";
                        break;
                    case Op::Abs:
                        o << b << " = fabs(" << b << ");";
                        break;
                    case Op::Smooth: // x = a, h = b
                        o << "if (" << b << " <= 0.0) " << a << " = " << a << " > 0.0 ? 1.0 : 0.0; else { const "
                          << "double r = (" << a << " + " << b << ") / (" << b << " + " << b << "); " << a
                          << " = r <= 0.0 ? 0.0 : (r >= 1.0 ? 1.0 : r); }";
                        break;
                    case Op::Eq:
                    case Op::Ne:
                    case Op::Gt:
                    case Op::Ge:
                    case Op::Lt:
                    case Op::Le:
                        o << a << " = (" << a << " " << crisp_cmp(in.op) << " " << b << ") ? 1.0 : 0.0;";
                        break;
                    case Op::And:
                        o << a << " = (" << a << " != 0.0) && (" << b << " != 0.0) ? 1.0 : 0.0;";
                        break;
                    case Op::Or:
                        o << a << " = (" << a << " != 0.0) || (" << b << " != 0.0) ? 1.0 : 0.0;";
                        break;
                    case Op::Not:
                        o << b << " = (" << b << " != 0.0) ? 0.0 : 1.0;";
                        break;

                    case Op::FCmpEq:
                    case Op::FCmpNe:
                    case Op::FCmpGt:
                    case Op::FCmpGe:
                    case Op::FCmpLt:
                    case Op::FCmpLe:
                    {
                        o << "{ const double x = __dsub_rn(" << a << ", " << b << "); " << d(dt) << " = ";
                        if (in.a != 0)
                            o << "(x " << hard_cmp(in.op) << " 0.0) ? 1.0 : 0.0;";
                        else
                        {
                            const std::string eps = lit(in.x), two = lit(2.0 * in.x);
                            switch (in.op)
                            {
                                case Op::FCmpLt:
                                case Op::FCmpLe:
                                    o << "1.0 - qm_up(x, " << eps << ", " << two << ");";
                                    break;
                                case Op::FCmpEq:
                                    o << "qm_tent(x, " << eps << ");";
                                    break;
                                case Op::FCmpNe:
                                    o << "1.0 - qm_tent(x, " << eps << ");";
                                    break;
                                default:
                                    o << "qm_up(x, " << eps << ", " << two << ");";
                            }
                        }
                        o << " }";
                        break;
                    }
                    case Op::FConst:
                        o << d(dt) << " = " << lit(in.x) << ";";
                        break;
                    case Op::FAnd: // std::min
                        o << d(dt - 2) << " = (" << d(dt - 1) << " < " << d(dt - 2) << ") ? " << d(dt - 1) << " : "
                          << d(dt - 2) << ";";
                        break;
                    case Op::FOr: // std::max
                        o << d(dt - 2) << " = (" << d(dt - 2) << " < " << d(dt - 1) << ") ? " << d(dt - 1) << " : "
                          << d(dt - 2) << ";";
                        break;
                    case Op::FNot:
                        o << d(dt - 1) << " = 1.0 - " << d(dt - 1) << ";";
                        break;

                    case Op::Assign:
                        o << var(in.a) << " = " << b << ";";
                        break;
                    case Op::Pays:
                        o << "P += " << b << " / num;";
                        break;
                    case Op::JumpIfFalse:
                        o << "if (" << b << " == 0.0) goto @" << in.a - begin << ";";
                        break;
                    case Op::Jump:
                        o << "goto @" << in.a - begin << ";";
                        break;

                    case Op::FIfBegin: // the degree is D's top; else-only jumps to b
                    {
                        const FuzzyIf &f = p.ifs[static_cast<std::size_t>(in.a)];
                        o << "{ const double g = " << d(dt - 1) << ";\n"
                          << "          if (g <= 0.0) { " << mode(f.mode) << " = 1; goto @" << in.b - begin << "; }\n"
                          << "          if (g >= 1.0) " << mode(f.mode) << " = 0;\n"
                          << "          else { " << mode(f.mode) << " = 2; " << q(f.slot) << " = g; " << q(f.slot + 1)
                          << " = P;";
                        for (int i = 0; i < f.n; ++i)
                            o << " " << q(f.slot + 3 + i) << " = " << var(p.aff[static_cast<std::size_t>(f.first + i)])
                              << ";";
                        o << " } }";
                        break;
                    }
                    case Op::FIfMid: // end of then; then-only jumps to b
                    {
                        const FuzzyIf &f = p.ifs[static_cast<std::size_t>(in.a)];
                        o << "if (" << mode(f.mode) << " == 0) goto @" << in.b - begin << ";\n        {";
                        for (int i = 0; i < f.n; ++i)
                            o << " " << q(f.slot + 3 + f.n + i) << " = "
                              << var(p.aff[static_cast<std::size_t>(f.first + i)]) << ";";
                        o << " " << q(f.slot + 2) << " = P;";
                        for (int i = 0; i < f.n; ++i)
                            o << " " << var(p.aff[static_cast<std::size_t>(f.first + i)]) << " = " << q(f.slot + 3 + i)
                              << ";";
                        o << " P = " << q(f.slot + 1) << "; }";
                        break;
                    }
                    case Op::FIfEnd: // the blend, written as run_event_impl writes it
                    {
                        const FuzzyIf &f = p.ifs[static_cast<std::size_t>(in.a)];
                        o << "if (" << mode(f.mode) << " == 2) { const double degree = " << q(f.slot)
                          << "; const double payoff0 = " << q(f.slot + 1)
                          << "; const double payoff_else = P; const double w1 = 1.0 - degree;";
                        for (int i = 0; i < f.n; ++i)
                        {
                            const std::string vv = var(p.aff[static_cast<std::size_t>(f.first + i)]);
                            o << "\n            " << vv << " = degree * " << q(f.slot + 3 + f.n + i) << " + w1 * " << vv
                              << ";";
                        }
                        o << "\n            P = payoff0 + degree * (" << q(f.slot + 2)
                          << " - payoff0) + w1 * (payoff_else - payoff0); }";
                        break;
                    }
                }
                o << "\n";
            }
            if (target[static_cast<std::size_t>(end - begin)])
                o << "    @" << end - begin << ":;\n";
            o << "        *payoff = P;\n        return;\n";
            const std::string text = o.str();
            auto it = std::find(bodies.begin(), bodies.end(), text);
            if (it == bodies.end())
            {
                bodies.push_back(text);
                cases.emplace_back();
                it = bodies.end() - 1;
            }
            cases[static_cast<std::size_t>(it - bodies.begin())].push_back(static_cast<int>(ev));
        }
        // Events with the same body (a daily schedule's dates) share one
        // case: the code the instruction cache holds is the script's distinct
        // statements, not one copy per date.
        for (std::size_t g = 0; g < bodies.size(); ++g)
        {
            for (int ev : cases[g])
                src << "    case " << ev << ":\n";
            std::string text = bodies[g];
            const std::string label = "L" + std::to_string(g) + "_";
            for (std::size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + label.size()))
                text.replace(at, 1, label);
            src << "    {\n"
                << text << "    }\n";
        }
        src << "    default:\n        return;\n    }\n}\n";
        out.source = src.str();
        return out;
    }

    std::string cuda_spec_source(const CudaKernelSpec &spec)
    {
        std::ostringstream o;
        auto fn = [&](const char *name, int value)
        { o << "extern \"C\" __device__ int " << name << "() { return " << value << "; }\n"; };
        o << "\n// The request this kernel serves (CudaKernelSpec).\n";
        fn("qm_spec_kind", spec.model_kind);
        fn("qm_spec_n_assets", spec.n_assets);
        fn("qm_spec_sobol", spec.sobol ? 1 : 0);
        fn("qm_spec_stratified", spec.stratified ? 1 : 0);
        fn("qm_spec_importance", spec.importance ? 1 : 0);
        fn("qm_spec_antithetic", spec.antithetic ? 1 : 0);
        return o.str();
    }

} // namespace quantModeling::scripting
