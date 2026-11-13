#include "quantModeling/scripting/cuda_codegen.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

// The IR's CUDA backend for prices (blueprint/wp/27-ir-ad.md §4, lot L1):
// one `case` per distinct block, each SSA value a local double. The
// operations are the interpreter's, in its order; + - * go through
// __dadd_rn / __dsub_rn / __dmul_rn, which the device compiler never
// contracts into an FMA.

namespace quantModeling::scripting
{

    namespace
    {
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

        std::string x(std::int32_t i)
        {
            return "x" + std::to_string(i);
        }
        std::string m(std::int32_t i)
        {
            return "m" + std::to_string(i);
        }

        const char *cmp(ir::Op op)
        {
            switch (op)
            {
                case ir::Op::Eq:
                    return "==";
                case ir::Op::Ne:
                    return "!=";
                case ir::Op::Gt:
                    return ">";
                case ir::Op::Ge:
                    return ">=";
                case ir::Op::Lt:
                    return "<";
                default:
                    return "<=";
            }
        }

        constexpr const char *kPrelude = R"(// Generated from a script's IR (quantModeling, scripting/ir_cuda.cpp).
// Compiled with --fmad=false, as the interpreter; sums, differences and
// products go through __dadd_rn / __dsub_rn / __dmul_rn, never contracted.

static __device__ __forceinline__ double qm_clamp01(double v)
{
    if (v <= 0.0)
        return 0.0;
    if (v >= 1.0)
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

        /// One block's statements; `term(k)` is how kernel parameter k reads.
        void block_body(std::ostringstream &o, const ir::Block &b)
        {
            using ir::Op;
            const auto n = static_cast<std::int32_t>(b.code.size());
            // Every value up front: a phi reads values its blocks defined.
            int declared = 0;
            for (std::int32_t k = 0; k < n; ++k)
                if (ir::defines_value(b.code[static_cast<std::size_t>(k)].op))
                {
                    o << (declared == 0 ? "        double " : (declared % 16 == 0 ? ",\n            " : ", ")) << x(k);
                    ++declared;
                }
            if (declared > 0)
                o << ";\n";
            std::string pad = "        ";
            for (std::int32_t k = 0; k < n; ++k)
            {
                const ir::Inst &in = b.code[static_cast<std::size_t>(k)];
                const std::string a = in.a >= 0 ? x(in.a) : "", c = in.b >= 0 ? x(in.b) : "";
                if (in.op == Op::Else || in.op == Op::EndIf || in.op == Op::FuzzyElse || in.op == Op::FuzzyEnd)
                    pad.resize(pad.size() - 4);
                o << pad;
                switch (in.op)
                {
                    case Op::Const:
                        o << x(k) << " = " << lit(in.x) << ";";
                        break;
                    case Op::Param:
                        o << x(k) << " = term[" << in.a << "];";
                        break;
                    case Op::VarIn:
                        o << x(k) << " = v[" << in.a << "];";
                        break;
                    case Op::PayIn:
                        o << x(k) << " = *payoff;";
                        break;
                    case Op::Spot:
                        o << x(k) << " = spot[" << in.a << "];";
                        break;
                    case Op::Df:
                        o << x(k) << " = df[" << in.a << "];";
                        break;
                    case Op::Surv:
                        o << x(k) << " = surv[" << in.a << "];";
                        break;
                    case Op::Num:
                        o << x(k) << " = num;";
                        break;
                    case Op::Add:
                        o << x(k) << " = __dadd_rn(" << a << ", " << c << ");";
                        break;
                    case Op::Sub:
                        o << x(k) << " = __dsub_rn(" << a << ", " << c << ");";
                        break;
                    case Op::Mul:
                        o << x(k) << " = __dmul_rn(" << a << ", " << c << ");";
                        break;
                    case Op::Div:
                        o << x(k) << " = " << a << " / " << c << ";";
                        break;
                    case Op::Pow:
                        o << x(k) << " = pow(" << a << ", " << c << ");";
                        break;
                    case Op::Neg:
                        o << x(k) << " = -" << a << ";";
                        break;
                    case Op::Min:
                        o << x(k) << " = (" << c << " < " << a << ") ? " << c << " : " << a << ";";
                        break;
                    case Op::Max:
                        o << x(k) << " = (" << a << " < " << c << ") ? " << c << " : " << a << ";";
                        break;
                    case Op::Log:
                        o << x(k) << " = log(" << a << ");";
                        break;
                    case Op::Exp:
                        o << x(k) << " = exp(" << a << ");";
                        break;
                    case Op::Sqrt:
                        o << x(k) << " = sqrt(" << a << ");";
                        break;
                    case Op::Abs:
                        o << x(k) << " = fabs(" << a << ");";
                        break;
                    case Op::Smooth:
                        o << "if (" << c << " <= 0.0) " << x(k) << " = " << a << " > 0.0 ? 1.0 : 0.0; else " << x(k)
                          << " = qm_clamp01((" << a << " + " << c << ") / (" << c << " + " << c << "));";
                        break;
                    case Op::Eq:
                    case Op::Ne:
                    case Op::Gt:
                    case Op::Ge:
                    case Op::Lt:
                    case Op::Le:
                        o << x(k) << " = (" << a << " " << cmp(in.op) << " " << c << ") ? 1.0 : 0.0;";
                        break;
                    case Op::And:
                        o << x(k) << " = (" << a << " != 0.0) && (" << c << " != 0.0) ? 1.0 : 0.0;";
                        break;
                    case Op::Or:
                        o << x(k) << " = (" << a << " != 0.0) || (" << c << " != 0.0) ? 1.0 : 0.0;";
                        break;
                    case Op::Not:
                        o << x(k) << " = (" << a << " != 0.0) ? 0.0 : 1.0;";
                        break;
                    case Op::Up:
                        o << x(k) << " = qm_up(" << a << ", " << lit(in.x) << ", " << lit(2.0 * in.x) << ");";
                        break;
                    case Op::Tent:
                        o << x(k) << " = qm_tent(" << a << ", " << lit(in.x) << ");";
                        break;

                    case Op::If:
                        o << "if (" << a << " != 0.0)\n"
                          << pad << "{";
                        pad += "    ";
                        break;
                    case Op::Else:
                        o << "}\n"
                          << pad << "else\n"
                          << pad << "{";
                        pad += "    ";
                        break;
                    case Op::EndIf:
                        o << "}";
                        break;
                    case Op::Phi:
                        o << x(k) << " = (" << x(b.code[static_cast<std::size_t>(in.c)].a) << " != 0.0) ? " << a
                          << " : " << c << ";";
                        break;

                    case Op::FuzzyIf: // 0: then only, 1: else only, 2: both, blended
                        o << "const int " << m(k) << " = " << a << " <= 0.0 ? 1 : (" << a << " >= 1.0 ? 0 : 2);\n"
                          << pad << "if (" << m(k) << " != 1)\n"
                          << pad << "{";
                        pad += "    ";
                        break;
                    case Op::FuzzyElse:
                        o << "}\n"
                          << pad << "if (" << m(in.a) << " != 0)\n"
                          << pad << "{";
                        pad += "    ";
                        break;
                    case Op::FuzzyEnd:
                        o << "}";
                        break;
                    case Op::FuzzyPhi:
                    {
                        const std::string d = x(b.code[static_cast<std::size_t>(in.c)].a);
                        o << x(k) << " = " << m(in.c) << " == 0 ? " << a << " : (" << m(in.c) << " == 1 ? " << c
                          << " : __dadd_rn(__dmul_rn(" << d << ", " << a << "), __dmul_rn(__dsub_rn(1.0, " << d
                          << "), " << c << ")));";
                        break;
                    }
                    case Op::FuzzyPhiPay:
                    {
                        const ir::Inst &f = b.code[static_cast<std::size_t>(in.c)];
                        const std::string d = x(f.a), p0 = x(f.b);
                        o << x(k) << " = " << m(in.c) << " == 0 ? " << a << " : (" << m(in.c) << " == 1 ? " << c
                          << " : __dadd_rn(__dadd_rn(" << p0 << ", __dmul_rn(" << d << ", __dsub_rn(" << a << ", "
                          << p0 << "))), __dmul_rn(__dsub_rn(1.0, " << d << "), __dsub_rn(" << c << ", " << p0
                          << "))));";
                        break;
                    }
                }
                o << "\n";
            }
            for (std::size_t i = 0; i < b.var_out.size(); ++i)
                if (b.var_out[i] >= 0)
                    o << "        v[" << i << "] = " << x(b.var_out[i]) << ";\n";
            if (b.pay_out >= 0)
                o << "        *payoff = " << x(b.pay_out) << ";\n";
            o << "        return;\n";
        }
    } // namespace

    CudaSource cuda_event_source(const ir::Script &s)
    {
        CudaSource out;
        if (s.empty())
        {
            out.why = "the script is not compiled";
            return out;
        }
        if (s.has_survival)
        {
            out.why = "survivalAbove() / survivalBelow() run on the CPU: the GPU paths do not compute the "
                      "bridge survival probabilities yet";
            return out;
        }
        std::ostringstream src;
        src << kPrelude;
        src << "extern \"C\" __device__ void " << kCudaEventFunction
            << "(int e, double *__restrict__ v, const double *__restrict__ spot, const double *__restrict__ df,\n"
            << "                                    double num, double *__restrict__ payoff)\n{\n"
            << "    switch (e)\n    {\n";
        // Events with the same block (a schedule's dates) share one case:
        // the code the instruction cache holds is the script's distinct
        // statements, not one copy per date.
        for (std::size_t g = 0; g < s.blocks.size(); ++g)
        {
            for (std::size_t e = 0; e < s.event_block.size(); ++e)
                if (s.event_block[e] == static_cast<std::int32_t>(g))
                    src << "    case " << e << ":\n";
            src << "    {\n";
            block_body(src, s.blocks[g]);
            src << "    }\n";
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
