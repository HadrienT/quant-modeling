#include "quantModeling/scripting/ir.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

namespace quantModeling::scripting::ir
{

    // ── builder ────────────────────────────────────────────────────────────

    namespace
    {
        std::uint64_t bits_of(double x)
        {
            std::uint64_t u = 0;
            std::memcpy(&u, &x, sizeof u);
            return u;
        }

        /// The fold of `op` on constants, where IEEE 754 rounds it correctly
        /// (the same double on every machine); false: leave it to run time.
        bool fold(Op op, double a, double b, double &r)
        {
            switch (op)
            {
                case Op::Add:
                    r = a + b;
                    return true;
                case Op::Sub:
                    r = a - b;
                    return true;
                case Op::Mul:
                    r = a * b;
                    return true;
                case Op::Div:
                    r = a / b;
                    return true;
                case Op::Neg:
                    r = -a;
                    return true;
                case Op::Sqrt:
                    r = std::sqrt(a);
                    return true;
                case Op::Abs:
                    r = std::fabs(a);
                    return true;
                case Op::Min:
                    r = (b < a) ? b : a;
                    return true;
                case Op::Max:
                    r = (a < b) ? b : a;
                    return true;
                default:
                    return false;
            }
        }
    } // namespace

    void Builder::open_scope()
    {
        scopes_.emplace_back();
    }

    std::int32_t Builder::raw(Op op, std::int32_t a, std::int32_t b, std::int32_t c, double x)
    {
        block_.code.push_back(Inst{op, a, b, c, x});
        return static_cast<std::int32_t>(block_.code.size()) - 1;
    }

    std::int32_t Builder::value(Op op, std::int32_t a, std::int32_t b, double x)
    {
        const unsigned operands = value_operands(op);
        if (operands != 0u)
        {
            const Inst &ia = at(a);
            const bool unary = operands == 1u;
            if (ia.op == Op::Const && (unary || at(b).op == Op::Const))
            {
                double r = 0.0;
                if (fold(op, ia.x, unary ? 0.0 : at(b).x, r))
                    return constant(r);
            }
        }
        const Key key{static_cast<int>(op), a, b, bits_of(x)};
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
        {
            const auto it = scope->find(key);
            if (it != scope->end())
                return it->second;
        }
        const std::int32_t id = raw(op, a, b, -1, x);
        scopes_.back().emplace(key, id);
        return id;
    }

    // ── dead code ──────────────────────────────────────────────────────────

    void remove_dead_code(Block &b)
    {
        const std::size_t n = b.code.size();
        std::vector<char> live(n, 0);
        std::vector<std::int32_t> work;
        const auto mark = [&](std::int32_t i)
        {
            if (i >= 0 && !live[static_cast<std::size_t>(i)])
            {
                live[static_cast<std::size_t>(i)] = 1;
                work.push_back(i);
            }
        };
        for (std::int32_t v : b.var_out)
            mark(v);
        mark(b.pay_out);
        while (!work.empty())
        {
            const std::int32_t i = work.back();
            work.pop_back();
            const Inst &in = b.code[static_cast<std::size_t>(i)];
            const unsigned operands = value_operands(in.op);
            if (operands & 1u)
                mark(in.a);
            if (operands & 2u)
                mark(in.b);
            switch (in.op)
            {
                case Op::Phi:
                case Op::FuzzyPhi:
                case Op::FuzzyPhiPay:
                    mark(in.c); // its region
                    break;
                case Op::If: // with its markers
                    mark(in.b);
                    mark(in.c);
                    break;
                case Op::FuzzyIf:
                    mark(in.c);
                    mark(b.code[static_cast<std::size_t>(in.c)].c);
                    break;
                default:
                    break;
            }
        }
        std::vector<std::int32_t> to(n, -1);
        std::vector<Inst> kept;
        for (std::size_t i = 0; i < n; ++i)
            if (live[i])
            {
                to[i] = static_cast<std::int32_t>(kept.size());
                kept.push_back(b.code[i]);
            }
        const auto map = [&](std::int32_t &i)
        {
            if (i >= 0)
                i = to[static_cast<std::size_t>(i)];
        };
        for (Inst &in : kept)
        {
            const unsigned operands = value_operands(in.op);
            const bool region = !defines_value(in.op);
            const bool phi = in.op == Op::Phi || in.op == Op::FuzzyPhi || in.op == Op::FuzzyPhiPay;
            if ((operands & 1u) || in.op == Op::Else || in.op == Op::EndIf || in.op == Op::FuzzyElse ||
                in.op == Op::FuzzyEnd)
                map(in.a);
            if ((operands & 2u) || in.op == Op::If)
                map(in.b);
            if (region || phi)
                map(in.c);
        }
        b.code = std::move(kept);
        for (std::int32_t &v : b.var_out)
            map(v);
        map(b.pay_out);
    }

    // ── lowering ───────────────────────────────────────────────────────────

    namespace
    {
        /// The tree to IR, statement by statement: the instructions the
        /// bytecode compiler (scripting/compiler.hpp) emits, in its order,
        /// with the stack's slots as named values and the variables renamed
        /// at each assignment.
        class Lowering final : public ConstVisitor
        {
          public:
            Lowering(std::size_t n_vars, bool fuzzy)
                : n_vars_(n_vars), fuzzy_(fuzzy) {}

            Block lower(const Event &event)
            {
                b_ = Builder();
                b_.block().var_out.assign(n_vars_, -1);
                vars_.resize(n_vars_);
                first_.resize(n_vars_);
                for (std::size_t i = 0; i < n_vars_; ++i)
                    vars_[i] = first_[i] = b_.raw(Op::VarIn, static_cast<std::int32_t>(i));
                pay_ = first_pay_ = b_.raw(Op::PayIn);
                values_.clear();
                degrees_.clear();
                has_survival_ = false;
                for (const ExprTree &statement : event.statements)
                    if (statement)
                        statement->accept(*this);
                Block out = b_.take();
                for (std::size_t i = 0; i < n_vars_; ++i)
                    out.var_out[i] = vars_[i] == first_[i] ? -1 : vars_[i];
                out.pay_out = pay_ == first_pay_ ? -1 : pay_;
                remove_dead_code(out);
                return out;
            }

            bool has_survival() const { return has_survival_; }

            // ── leaves ──────────────────────────────────────────────────────
            void visit(const NodeConst &n) override { push(b_.constant(n.value)); }
            void visit(const NodeVar &n) override { push(vars_.at(n.index)); }
            void visit(const NodeSpot &n) override
            {
                const auto i = static_cast<std::int32_t>(n.index);
                b_.block().max_spot = std::max(b_.block().max_spot, i);
                push(b_.value(Op::Spot, i));
            }
            void visit(const NodeDf &n) override
            {
                if (n.slot == NodeDf::unindexed)
                    throw InvalidInput("df(" + n.date.to_iso() +
                                       "): not resolved -- DiscountLookupResolver must run before lowering");
                const auto i = static_cast<std::int32_t>(n.slot);
                b_.block().max_df = std::max(b_.block().max_df, i);
                push(b_.value(Op::Df, i));
            }
            void visit(const NodeSurvival &n) override
            {
                if (n.slot == NodeSurvival::unindexed)
                    throw InvalidInput("survivalAbove/Below(): not resolved -- BarrierLookupResolver must run "
                                       "before lowering");
                const auto i = static_cast<std::int32_t>(n.slot);
                b_.block().max_surv = std::max(b_.block().max_surv, i);
                has_survival_ = true;
                push(b_.value(Op::Surv, i));
            }

            // ── arithmetic and functions ────────────────────────────────────
            void visit(const NodeAdd &n) override { binary(n, Op::Add); }
            void visit(const NodeSub &n) override { binary(n, Op::Sub); }
            void visit(const NodeMult &n) override { binary(n, Op::Mul); }
            void visit(const NodeDiv &n) override { binary(n, Op::Div); }
            void visit(const NodePow &n) override { binary(n, Op::Pow); }
            void visit(const NodeUplus &n) override { n.arguments[0]->accept(*this); }
            void visit(const NodeUminus &n) override { unary(n, Op::Neg); }
            void visit(const NodeMin &n) override { binary(n, Op::Min); }
            void visit(const NodeMax &n) override { binary(n, Op::Max); }
            void visit(const NodeLog &n) override { unary(n, Op::Log); }
            void visit(const NodeExp &n) override { unary(n, Op::Exp); }
            void visit(const NodeSqrt &n) override { unary(n, Op::Sqrt); }
            void visit(const NodeAbs &n) override { unary(n, Op::Abs); }
            void visit(const NodeSmooth &n) override { binary(n, Op::Smooth); }

            // ── logic ───────────────────────────────────────────────────────
            void visit(const NodeEqual &n) override { comparison(n, Op::Eq); }
            void visit(const NodeNotEqual &n) override { comparison(n, Op::Ne); }
            void visit(const NodeSuperior &n) override { comparison(n, Op::Gt); }
            void visit(const NodeSupEqual &n) override { comparison(n, Op::Ge); }
            void visit(const NodeInferior &n) override { comparison(n, Op::Lt); }
            void visit(const NodeInfEqual &n) override { comparison(n, Op::Le); }
            void visit(const NodeExercise &) override
            {
                throw InvalidInput("script IR: exercise() and call() run on the tree evaluator only");
            }
            void visit(const NodeAnd &n) override { connective(n, Op::And, Op::Min); }
            void visit(const NodeOr &n) override { connective(n, Op::Or, Op::Max); }
            void visit(const NodeNot &n) override
            {
                n.arguments[0]->accept(*this);
                if (fuzzy_)
                    push_degree(b_.value(Op::Sub, b_.constant(1.0), pop_degree()));
                else
                    push(b_.value(Op::Not, pop()));
            }

            // ── statements ──────────────────────────────────────────────────
            void visit(const NodeAssign &n) override
            {
                n.arguments[1]->accept(*this);
                vars_.at(static_cast<const NodeVar &>(*n.arguments[0]).index) = pop();
            }
            void visit(const NodePays &n) override
            {
                n.arguments[0]->accept(*this);
                const std::int32_t value = pop();
                pay_ = b_.value(Op::Add, pay_, b_.value(Op::Div, value, b_.value(Op::Num)));
            }
            /// variables[c] = variables[c] + amount / numeraire.
            void visit(const NodeControlPays &n) override
            {
                const std::size_t index = static_cast<const NodeVar &>(*n.arguments[0]).index;
                n.arguments[1]->accept(*this);
                const std::int32_t amount = pop();
                vars_.at(index) = b_.value(Op::Add, vars_.at(index), b_.value(Op::Div, amount, b_.value(Op::Num)));
            }
            void visit(const NodeIf &n) override
            {
                const auto *cond = dynamic_cast<const NodeComparison *>(n.arguments[0].get());
                if (cond && cond->alwaysTrue)
                    return statements(n, 1, n.firstElse);
                if (cond && cond->alwaysFalse)
                    return statements(n, n.firstElse, n.arguments.size());

                n.arguments[0]->accept(*this);
                const std::vector<std::int32_t> before = vars_;
                const std::int32_t pay_before = pay_;
                const std::int32_t head =
                    fuzzy_ ? b_.raw(Op::FuzzyIf, pop_degree(), pay_before) : b_.raw(Op::If, pop());

                b_.open_scope();
                statements(n, 1, n.firstElse);
                b_.close_scope();
                const std::vector<std::int32_t> then_vars = vars_;
                const std::int32_t then_pay = pay_;

                const std::int32_t mid = b_.raw(fuzzy_ ? Op::FuzzyElse : Op::Else, head);
                vars_ = before;
                pay_ = pay_before;
                b_.open_scope();
                statements(n, n.firstElse, n.arguments.size());
                b_.close_scope();
                const std::int32_t end = b_.raw(fuzzy_ ? Op::FuzzyEnd : Op::EndIf, head);
                b_.at(mid).c = end;
                if (fuzzy_)
                    b_.at(head).c = mid;
                else
                {
                    b_.at(head).b = mid;
                    b_.at(head).c = end;
                }

                if (!fuzzy_)
                {
                    for (std::size_t i = 0; i < n_vars_; ++i)
                        if (then_vars[i] != vars_[i])
                            vars_[i] = b_.raw(Op::Phi, then_vars[i], vars_[i], head);
                    if (then_pay != pay_)
                        pay_ = b_.raw(Op::Phi, then_pay, pay_, head);
                    return;
                }
                // The interpreter blends every variable either block writes
                // (NodeIf::affectedVars), in that order, then the payoff.
                std::vector<char> affected(n_vars_, 0);
                for (std::size_t slot : n.affectedVars)
                    affected.at(slot) = 1;
                for (std::size_t i = 0; i < n_vars_; ++i)
                    if (!affected[i] && (then_vars[i] != before[i] || vars_[i] != before[i]))
                        throw InvalidInput("script IR: a fuzzy if writes a variable its IfProcessor did not list");
                for (std::size_t slot : n.affectedVars)
                    vars_[slot] = b_.raw(Op::FuzzyPhi, then_vars[slot], vars_[slot], head);
                // No payment in either block: the blend would be
                // payoff0 + d (payoff0 - payoff0) + (1 - d) (payoff0 - payoff0).
                if (then_pay != pay_before || pay_ != pay_before)
                    pay_ = b_.raw(Op::FuzzyPhiPay, then_pay, pay_, head);
                else
                    b_.at(head).b = -1;
            }
            void visit(const NodeCollect &n) override { n.arguments[0]->accept(*this); }

          private:
            void push(std::int32_t v) { values_.push_back(v); }
            std::int32_t pop()
            {
                if (values_.empty())
                    throw InvalidInput("script compiler: a number is read where only a condition was computed "
                                       "(in fuzzy mode, comparisons are degrees of truth, not numbers)");
                const std::int32_t v = values_.back();
                values_.pop_back();
                return v;
            }
            void push_degree(std::int32_t v) { degrees_.push_back(v); }
            std::int32_t pop_degree()
            {
                if (degrees_.empty())
                    throw InvalidInput("script compiler: a condition is expected where a number was computed");
                const std::int32_t v = degrees_.back();
                degrees_.pop_back();
                return v;
            }

            void binary(const Node &n, Op op)
            {
                n.arguments[0]->accept(*this);
                n.arguments[1]->accept(*this);
                const std::int32_t rhs = pop();
                const std::int32_t lhs = pop();
                push(b_.value(op, lhs, rhs));
            }
            void unary(const Node &n, Op op)
            {
                n.arguments[0]->accept(*this);
                push(b_.value(op, pop()));
            }
            void comparison(const NodeComparison &n, Op op)
            {
                if (!fuzzy_)
                    return binary(n, op);
                if (n.alwaysTrue || n.alwaysFalse)
                    return push_degree(b_.constant(n.alwaysTrue ? 1.0 : 0.0));
                n.arguments[0]->accept(*this);
                n.arguments[1]->accept(*this);
                const std::int32_t rhs = pop();
                const std::int32_t lhs = pop();
                const std::int32_t x = b_.value(Op::Sub, lhs, rhs); // the condition is x ⋈ 0
                if (n.discrete || n.eps <= 0.0)
                    return push_degree(b_.value(op, x, b_.constant(0.0)));
                switch (op)
                {
                    case Op::Lt:
                    case Op::Le:
                        return push_degree(b_.value(Op::Sub, b_.constant(1.0), b_.value(Op::Up, x, -1, n.eps)));
                    case Op::Eq:
                        return push_degree(b_.value(Op::Tent, x, -1, n.eps));
                    case Op::Ne:
                        return push_degree(b_.value(Op::Sub, b_.constant(1.0), b_.value(Op::Tent, x, -1, n.eps)));
                    default:
                        return push_degree(b_.value(Op::Up, x, -1, n.eps));
                }
            }
            void connective(const Node &n, Op hard_op, Op fuzzy_op)
            {
                n.arguments[0]->accept(*this);
                n.arguments[1]->accept(*this);
                if (fuzzy_)
                {
                    const std::int32_t rhs = pop_degree();
                    const std::int32_t lhs = pop_degree();
                    return push_degree(b_.value(fuzzy_op, lhs, rhs));
                }
                const std::int32_t rhs = pop();
                const std::int32_t lhs = pop();
                push(b_.value(hard_op, lhs, rhs));
            }
            void statements(const NodeIf &n, std::size_t begin, std::size_t end)
            {
                for (std::size_t k = begin; k < end; ++k)
                    n.arguments[k]->accept(*this);
            }

            std::size_t n_vars_;
            bool fuzzy_;
            Builder b_;
            std::vector<std::int32_t> vars_, first_; ///< each variable's current value, and its VarIn
            std::int32_t pay_ = -1, first_pay_ = -1;
            std::vector<std::int32_t> values_, degrees_;
            bool has_survival_ = false;
        };
    } // namespace

    Script lower_script(const std::vector<Event> &events, std::size_t n_vars, bool fuzzy)
    {
        Script s;
        s.n_vars = static_cast<int>(n_vars);
        s.fuzzy = fuzzy;
        Lowering lowering(n_vars, fuzzy);
        for (const Event &event : events)
        {
            Block b = lowering.lower(event);
            s.has_survival = s.has_survival || lowering.has_survival();
            auto it = std::find(s.blocks.begin(), s.blocks.end(), b);
            if (it == s.blocks.end())
            {
                s.blocks.push_back(std::move(b));
                it = s.blocks.end() - 1;
            }
            s.event_block.push_back(static_cast<std::int32_t>(it - s.blocks.begin()));
        }
        return s;
    }

    // ── printer ────────────────────────────────────────────────────────────

    namespace
    {
        const char *name(Op op)
        {
            switch (op)
            {
                case Op::Const:
                    return "const";
                case Op::Param:
                    return "param";
                case Op::VarIn:
                    return "var_in";
                case Op::PayIn:
                    return "pay_in";
                case Op::Spot:
                    return "load_spot";
                case Op::Df:
                    return "load_df";
                case Op::Surv:
                    return "load_survival";
                case Op::Num:
                    return "load_numeraire";
                case Op::Add:
                    return "add";
                case Op::Sub:
                    return "sub";
                case Op::Mul:
                    return "mul";
                case Op::Div:
                    return "div";
                case Op::Pow:
                    return "pow";
                case Op::Neg:
                    return "neg";
                case Op::Min:
                    return "min";
                case Op::Max:
                    return "max";
                case Op::Log:
                    return "log";
                case Op::Exp:
                    return "exp";
                case Op::Sqrt:
                    return "sqrt";
                case Op::Abs:
                    return "abs";
                case Op::Smooth:
                    return "smooth";
                case Op::Eq:
                    return "cmp_eq";
                case Op::Ne:
                    return "cmp_ne";
                case Op::Gt:
                    return "cmp_gt";
                case Op::Ge:
                    return "cmp_ge";
                case Op::Lt:
                    return "cmp_lt";
                case Op::Le:
                    return "cmp_le";
                case Op::And:
                    return "and";
                case Op::Or:
                    return "or";
                case Op::Not:
                    return "not";
                case Op::Up:
                    return "ramp_up";
                case Op::Tent:
                    return "ramp_tent";
                case Op::If:
                    return "if";
                case Op::Else:
                    return "else";
                case Op::EndIf:
                    return "end_if";
                case Op::Phi:
                    return "phi";
                case Op::FuzzyIf:
                    return "fuzzy_if";
                case Op::FuzzyElse:
                    return "fuzzy_else";
                case Op::FuzzyEnd:
                    return "fuzzy_end";
                case Op::FuzzyPhi:
                    return "fuzzy_phi";
                case Op::FuzzyPhiPay:
                    return "fuzzy_phi_pay";
            }
            return "?";
        }

        std::string number(double x)
        {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", x);
            return buf;
        }
    } // namespace

    std::string to_string(const Block &b)
    {
        std::ostringstream o;
        int indent = 0;
        for (std::size_t k = 0; k < b.code.size(); ++k)
        {
            const Inst &in = b.code[k];
            const bool closes = in.op == Op::Else || in.op == Op::EndIf || in.op == Op::FuzzyElse ||
                                in.op == Op::FuzzyEnd;
            if (closes)
                --indent;
            o << std::string(static_cast<std::size_t>(2 * (indent + 1)), ' ');
            if (defines_value(in.op))
                o << '%' << k << " = ";
            o << name(in.op);
            const unsigned operands = value_operands(in.op);
            switch (in.op)
            {
                case Op::Const:
                    o << ' ' << number(in.x);
                    break;
                case Op::Param:
                case Op::VarIn:
                case Op::Spot:
                case Op::Df:
                case Op::Surv:
                    o << ' ' << in.a;
                    break;
                case Op::FuzzyIf:
                    o << " %" << in.a;
                    break;
                default:
                    if (operands & 1u)
                        o << " %" << in.a;
                    if (operands & 2u)
                        o << ", %" << in.b;
                    if (in.op == Op::Up || in.op == Op::Tent)
                        o << ", eps " << number(in.x);
            }
            o << '\n';
            if (in.op == Op::If || in.op == Op::Else || in.op == Op::FuzzyIf || in.op == Op::FuzzyElse)
                ++indent;
        }
        for (std::size_t i = 0; i < b.var_out.size(); ++i)
            if (b.var_out[i] >= 0)
                o << "  var_out " << i << " = %" << b.var_out[i] << '\n';
        if (b.pay_out >= 0)
            o << "  pay_out = %" << b.pay_out << '\n';
        return o.str();
    }

    std::string to_string(const Script &s)
    {
        std::ostringstream o;
        for (std::size_t g = 0; g < s.blocks.size(); ++g)
        {
            o << "block " << g << " (events";
            for (std::size_t e = 0; e < s.event_block.size(); ++e)
                if (s.event_block[e] == static_cast<std::int32_t>(g))
                    o << ' ' << e;
            o << ")\n"
              << to_string(s.blocks[g]);
        }
        return o.str();
    }

} // namespace quantModeling::scripting::ir
