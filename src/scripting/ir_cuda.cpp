#include "quantModeling/scripting/cuda_codegen.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <sstream>
#include <vector>

#include "quantModeling/scripting/ir_ad.hpp"

// The IR's CUDA backend (blueprint/wp/27-ir-ad.md §4 and §6, lots L1-L4):
// every block of a path -- the state's start, the model's step, the script's
// events, and their tangents or adjoints -- as a device function on a
// QmFrame, each SSA value a local double. The operations are the IR's, in
// its order; sums, differences and products go through __dadd_rn /
// __dsub_rn / __dmul_rn, which the device compiler never contracts into an
// FMA.

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

        // QmFrame is src/gpu/script_units.cuh's: the two must be laid out the
        // same way.
        constexpr const char *kPrelude = R"(// Generated from a script's IR (quantModeling, scripting/ir_cuda.cpp).
// Compiled with --fmad=false, as the interpreter; sums, differences and
// products go through __dadd_rn / __dsub_rn / __dmul_rn, never contracted.

struct QmFrame
{
    double *st;             // the path's state: the model's, the script's variables, the payoff
    double *dst;            // its tangents, [slot x directions + d]
    double *ast;            // its adjoints
    double *lth;            // the scalar parameters' adjoints, this path's
    double *ath;            // the grid's adjoints, parameter p at ath[p x ath_stride]
    long ath_stride;
    const double *z;        // the step's gaussians
    const double *dat;      // the step's or the date's data row
    const double *th;       // the model's parameters
    const double *tm;       // the date's terms
    const double *gK;       // the local-vol grid's strikes and times
    const double *gT;
    int nK, nT, grid0, reserved;
    void *cell;             // the adjoint's backward sweep: the grid cell of the step
};

extern "C" __device__ double qm_grid_vol(const QmFrame *f, double spot, double t);
extern "C" __device__ double qm_grid_slope(const QmFrame *f, double spot, double t);
extern "C" __device__ void qm_grid_accumulate(const QmFrame *f, double spot, double t, double adjoint);
extern "C" __device__ double qm_grid_leverage(const QmFrame *f, double spot, double t);
extern "C" __device__ double qm_grid_leverage_slope(const QmFrame *f, double spot, double t);
extern "C" __device__ void qm_grid_leverage_accumulate(const QmFrame *f, double spot, double t, double adjoint);

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

        struct Emit
        {
            int K = 0;            ///< tangent directions
            int local_thetas = 0; ///< parameters below this accumulate in f->lth
        };

        std::string slot(const Emit &e, std::int32_t index, std::int32_t tag)
        {
            const int d = ir::direction_of(tag);
            if (ir::is_adjoint(tag))
            {
                if (d >= 0)
                    throw InvalidInput("script IR: second-order blocks have no CUDA backend yet");
                return "f->ast[" + std::to_string(index) + "]";
            }
            if (d < 0)
                return "f->st[" + std::to_string(index) + "]";
            return "f->dst[" + std::to_string(index * e.K + d) + "]";
        }

        /// One path-level block's statements.
        void block_body(std::ostringstream &o, const ir::Block &b, const Emit &e, const char *indent)
        {
            using ir::Op;
            const auto n = static_cast<std::int32_t>(b.code.size());
            // Every value up front, at 0: a phi and a reversed region read
            // values a block defined, and a Select may name one that did not
            // run.
            int declared = 0;
            for (std::int32_t k = 0; k < n; ++k)
                if (ir::defines_value(b.code[static_cast<std::size_t>(k)].op))
                {
                    o << (declared == 0 ? std::string(indent) + "double "
                                        : (declared % 8 == 0 ? ",\n" + std::string(indent) + "    " : ", "))
                      << x(k) << " = 0.0";
                    ++declared;
                }
            if (declared > 0)
                o << ";\n";
            std::string pad = indent;
            for (std::int32_t k = 0; k < n; ++k)
            {
                const ir::Inst &in = b.code[static_cast<std::size_t>(k)];
                const unsigned operands = ir::value_operands(in.op);
                const std::string a = (operands & 1u) && in.a >= 0 ? x(in.a) : "",
                                  c = (operands & 2u) && in.b >= 0 ? x(in.b) : "";
                if (in.op == Op::Else || in.op == Op::EndIf || in.op == Op::FuzzyElse || in.op == Op::FuzzyEnd)
                    pad.resize(pad.size() - 4);
                o << pad;
                switch (in.op)
                {
                    case Op::Const:
                        o << x(k) << " = " << lit(in.x) << ";";
                        break;
                    case Op::Param:
                        o << x(k) << " = f->tm[" << in.a << "];";
                        break;
                    case Op::State:
                        o << x(k) << " = " << slot(e, in.a, in.c) << ";";
                        break;
                    case Op::Draw:
                        o << x(k) << " = f->z[" << in.a << "];";
                        break;
                    case Op::Data:
                        o << x(k) << " = f->dat[" << in.a << "];";
                        break;
                    case Op::Theta:
                        o << x(k) << " = f->th[" << in.a << "];";
                        break;
                    case Op::VarIn:
                    case Op::PayIn:
                    case Op::Spot:
                    case Op::Df:
                    case Op::Surv:
                    case Op::Num:
                        throw InvalidInput("script IR: a script-level block reached the CUDA backend unbound");
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
                    case Op::Select:
                        o << x(k) << " = (" << a << " != 0.0) ? " << c << " : " << x(in.c) << ";";
                        break;
                    case Op::Up:
                        o << x(k) << " = qm_up(" << a << ", " << lit(in.x) << ", " << lit(2.0 * in.x) << ");";
                        break;
                    case Op::Tent:
                        o << x(k) << " = qm_tent(" << a << ", " << lit(in.x) << ");";
                        break;
                    case Op::GridVol:
                        o << x(k) << " = qm_grid_vol(f, " << a << ", " << c << ");";
                        break;
                    case Op::GridSlope:
                        o << x(k) << " = qm_grid_slope(f, " << a << ", " << c << ");";
                        break;
                    case Op::GridAccum:
                        o << "qm_grid_accumulate(f, " << c << ", " << x(in.c) << ", " << a << ");";
                        break;
                    case Op::GridLev:
                        o << x(k) << " = qm_grid_leverage(f, " << a << ", " << c << ");";
                        break;
                    case Op::GridLevSlope:
                        o << x(k) << " = qm_grid_leverage_slope(f, " << a << ", " << c << ");";
                        break;
                    case Op::GridLevAccum:
                        o << "qm_grid_leverage_accumulate(f, " << c << ", " << x(in.c) << ", " << a << ");";
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
                        const ir::Inst &fi = b.code[static_cast<std::size_t>(in.c)];
                        const std::string d = x(fi.a), p0 = x(fi.b);
                        o << x(k) << " = " << m(in.c) << " == 0 ? " << a << " : (" << m(in.c) << " == 1 ? " << c
                          << " : __dadd_rn(__dadd_rn(" << p0 << ", __dmul_rn(" << d << ", __dsub_rn(" << a << ", "
                          << p0 << "))), __dmul_rn(__dsub_rn(1.0, " << d << "), __dsub_rn(" << c << ", " << p0
                          << "))));";
                        break;
                    }
                }
                o << "\n";
            }
            for (const ir::Out &out : b.outs)
            {
                o << indent;
                if (out.cls == Op::State)
                    o << slot(e, out.index, out.tag) << (out.add ? " += " : " = ") << x(out.value) << ";\n";
                else if (out.cls == Op::Theta && out.index < e.local_thetas) // a scalar parameter's adjoint
                    o << "f->lth[" << out.index << "] = __dadd_rn(f->lth[" << out.index << "], " << x(out.value)
                      << ");\n";
                else if (out.cls == Op::Theta)
                    o << "f->ath[" << out.index << " * f->ath_stride] += " << x(out.value) << ";\n";
                else
                    throw InvalidInput("script IR: a script-level block reached the CUDA backend unbound");
            }
        }

        void function(std::ostringstream &o, const char *name, const ir::Block &b, const Emit &e)
        {
            o << "extern \"C\" __device__ void " << name << "(const QmFrame *__restrict__ f)\n{\n";
            block_body(o, b, e, "    ");
            o << "}\n\n";
        }

        /// One `case` per distinct block: which block a date runs is data,
        /// so the source does not depend on the number of dates.
        void events(std::ostringstream &o, const char *name, const std::vector<ir::Block> &blocks, const Emit &e)
        {
            o << "extern \"C\" __device__ void " << name << "(int kind, const QmFrame *__restrict__ f)\n{\n"
              << "    switch (kind)\n    {\n";
            for (std::size_t g = 0; g < blocks.size(); ++g)
            {
                o << "    case " << g << ":\n    {\n";
                block_body(o, blocks[g], e, "        ");
                o << "        return;\n    }\n";
            }
            o << "    default:\n        return;\n    }\n}\n\n";
        }

        void constant(std::ostringstream &o, const char *name, int value)
        {
            o << "extern \"C\" __device__ int " << name << "() { return " << value << "; }\n";
        }
    } // namespace

    CudaSource cuda_path_source(const ir::Script &script, int model_kind, int n_assets, PathKernel kernel)
    {
        CudaSource out;
        if (script.empty())
        {
            out.why = "the script is not compiled";
            return out;
        }
        if (script.has_survival)
        {
            out.why = "survivalAbove() / survivalBelow() run on the CPU: the GPU paths do not compute the "
                      "bridge survival probabilities yet";
            return out;
        }
        const auto kind = static_cast<DeviceModel::Kind>(model_kind);
        out.why = ir::path_program_unsupported(kind, n_assets);
        if (!out.why.empty())
            return out;

        const ir::PathProgram primal = ir::path_program(script, kind, n_assets, kernel != PathKernel::Price);
        const ir::PathLayout &l = primal.layout;
        std::ostringstream src;
        src << kPrelude;
        src << "// The path's shape (ir::PathLayout).\n";
        constant(src, "qm_n_state", l.n_state());
        constant(src, "qm_n_model", l.n_model);
        constant(src, "qm_pay_slot", l.pay());
        constant(src, "qm_factors", l.factors);
        constant(src, "qm_step_cols", l.step_cols);
        constant(src, "qm_event_cols", l.event_cols);
        constant(src, "qm_n_risks", l.n_risks);
        const int ns = l.n_state();
        src << "\n";
        Emit e;
        switch (kernel)
        {
            case PathKernel::Price:
                function(src, "qm_init", primal.init, e);
                function(src, "qm_step", primal.step, e);
                events(src, "qm_event", primal.events, e);
                src << "extern \"C\" __device__ double qm_drive(const void *unit, void *draws, double sign, double "
                       "*controls, double *st);\n"
                    << "extern \"C\" __device__ double qm_path(const void *unit, void *draws, double sign, double "
                       "*controls)\n{\n    double st["
                    << ns << "];\n    return qm_drive(unit, draws, sign, controls, st);\n}\n";
                break;
            case PathKernel::Tangent:
            {
                std::vector<std::int32_t> dirs(static_cast<std::size_t>(l.n_risks));
                std::iota(dirs.begin(), dirs.end(), 0);
                const ir::PathProgram tan = ir::tangent(primal, dirs);
                e.K = l.n_risks;
                function(src, "qm_init", tan.init, e);
                function(src, "qm_step", tan.step, e);
                events(src, "qm_event", tan.events, e);
                src << "extern \"C\" __device__ void qm_drive(const void *unit, void *draws, double *out, double "
                       "*st, double *dst);\n"
                    << "extern \"C\" __device__ void qm_path(const void *unit, void *draws, double *out)\n{\n"
                    << "    double st[" << ns << "];\n    double dst[" << ns * e.K << "];\n"
                    << "    qm_drive(unit, draws, out, st, dst);\n}\n";
                break;
            }
            case PathKernel::Adjoint:
            {
                const ir::PathProgram adj = ir::adjoint(primal);
                e.local_thetas = l.n_risks;
                function(src, "qm_init", primal.init, e);
                function(src, "qm_step", primal.step, e);
                events(src, "qm_event", primal.events, e);
                function(src, "qm_init_adjoint", adj.init, e);
                function(src, "qm_step_adjoint", adj.step, e);
                events(src, "qm_event_adjoint", adj.events, e);
                src << "extern \"C\" __device__ double qm_drive(const void *unit, void *draws, double *st, double "
                       "*ast, double *lth);\n"
                    << "extern \"C\" __device__ double qm_path(const void *unit, void *draws)\n{\n"
                    << "    double st[" << ns << "];\n    double ast[" << ns << "];\n    double lth[" << l.n_risks
                    << "];\n    return qm_drive(unit, draws, st, ast, lth);\n}\n";
                break;
            }
        }
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
        fn("qm_spec_controls", spec.controls);
        return o.str();
    }

} // namespace quantModeling::scripting
