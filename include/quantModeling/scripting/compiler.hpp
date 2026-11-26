#ifndef QM_SCRIPTING_COMPILER_HPP
#define QM_SCRIPTING_COMPILER_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "quantModeling/scripting/bytecode.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

namespace quantModeling::scripting
{

    /**
     * @brief Compiles the events of a processed script into a Program
     *        (blueprint/wp/19-gpu.md §3, lot G2).
     *
     * Runs after the front-end passes (VarIndexer, ConstCondProcessor,
     * IfProcessor, DomainProcessor, discount-lookup resolution). A ConstVisitor
     * that emits, for every node, the instructions the tree evaluator would
     * execute in the same order: children left to right, then the operation.
     * `fuzzy` selects FuzzyEvaluator's semantics (comparisons as degrees,
     * blended `if`s) instead of Evaluator's.
     *
     * Stack depths are tracked at compile time, so the machine's buffers are
     * sized once; a script that would pop a value it never pushed -- in fuzzy
     * mode, a comparison used as a number -- is refused here rather than read
     * out of bounds at run time.
     */
    class Compiler final : public ConstVisitor
    {
      public:
        /// `n_spots`: the script's underlyings, the regressors of an
        /// exercise() / call() without arguments.
        Program compile(const std::vector<Event> &events, std::size_t n_vars, bool fuzzy, bool smooth_ramp = false,
                        std::size_t n_spots = 0)
        {
            p_ = Program{};
            p_.fuzzy = fuzzy;
            smooth_ = smooth_ramp;
            n_spots_ = static_cast<std::int32_t>(n_spots);
            p_.n_vars = static_cast<int>(n_vars);
            event_ = -1;
            for (const Event &event : events)
            {
                ++event_;
                p_.event_begin.push_back(pc());
                max_spot_ = max_df_ = max_surv_ = max_fwd_ = -1;
                slot_ = mode_ = 0; // an event\'s ifs reuse the previous event\'s slots
                for (const ExprTree &statement : event.statements)
                    if (statement)
                        statement->accept(*this);
                p_.event_max_spot.push_back(max_spot_);
                p_.event_max_df.push_back(max_df_);
                p_.event_max_surv.push_back(max_surv_);
                p_.event_max_fwd.push_back(max_fwd_);
            }
            p_.event_begin.push_back(pc());
            return std::move(p_);
        }

        // ── leaves ──────────────────────────────────────────────────────────
        void visit(const NodeConst &n) override { emit(Op::Const, 0, 0, n.value), push(); }
        void visit(const NodeVar &n) override
        {
            emit(Op::Var, static_cast<std::int32_t>(n.index)), push();
        }
        void visit(const NodeSpot &n) override
        {
            max_spot_ = std::max(max_spot_, static_cast<std::int32_t>(n.index));
            emit(Op::Spot, static_cast<std::int32_t>(n.index)), push();
        }
        void visit(const NodeDf &n) override
        {
            if (n.slot == NodeDf::unindexed)
                throw InvalidInput("df(" + n.date.to_iso() +
                                   "): not resolved -- DiscountLookupResolver must run before compiling");
            max_df_ = std::max(max_df_, static_cast<std::int32_t>(n.slot));
            emit(Op::Df, static_cast<std::int32_t>(n.slot)), push();
        }
        void visit(const NodeFwd &n) override
        {
            if (n.slot == NodeFwd::unindexed)
                throw InvalidInput("fwd(" + n.date.to_iso() +
                                   "): not resolved -- ForwardLookupResolver must run before compiling");
            max_fwd_ = std::max(max_fwd_, static_cast<std::int32_t>(n.slot));
            p_.has_forward = true;
            emit(Op::Fwd, static_cast<std::int32_t>(n.slot)), push();
        }
        // yf() is a number once resolved
        void visit(const NodeYf &n) override
        {
            if (!n.resolved)
                throw InvalidInput("yf(" + n.from.to_iso() + ", " + n.to.to_iso() +
                                   "): not resolved -- DiscountLookupResolver must run before compiling");
            emit(Op::Const, 0, 0, n.value), push();
        }
        void visit(const NodeStepQuantity &n) override
        {
            if (n.slot == NodeStepQuantity::unindexed)
                throw InvalidInput("survivalAbove/Below(): not resolved -- StepMonitorResolver must run before "
                                   "compiling");
            max_surv_ = std::max(max_surv_, static_cast<std::int32_t>(n.slot));
            p_.has_monitor = true;
            p_.has_bridge = p_.has_bridge || n.kind == StepQuantity::Survival;
            p_.has_knock_out = p_.has_knock_out || n.kind == StepQuantity::KnockOut;
            if (n.computed_strike()) // the script's strike, then the step's forward and variance (issue #363)
            {
                max_surv_ = std::max(max_surv_, static_cast<std::int32_t>(n.var_slot));
                n.arguments[0]->accept(*this);
                emit(Op::Surv, static_cast<std::int32_t>(n.slot)), push();
                emit(Op::Surv, static_cast<std::int32_t>(n.var_slot)), push();
                emit(Op::LastStep, static_cast<std::int32_t>(n.kind)), pop(), pop();
                return;
            }
            emit(Op::Surv, static_cast<std::int32_t>(n.slot)), push();
        }

        // ── arithmetic and functions ────────────────────────────────────────
        void visit(const NodeAdd &n) override { binary(n, Op::Add); }
        void visit(const NodeSub &n) override { binary(n, Op::Sub); }
        void visit(const NodeMult &n) override { binary(n, Op::Mul); }
        void visit(const NodeDiv &n) override { binary(n, Op::Div); }
        void visit(const NodePow &n) override { binary(n, Op::Pow); }
        void visit(const NodeUplus &n) override { unary(n, Op::Uplus); }
        void visit(const NodeUminus &n) override { unary(n, Op::Neg); }
        void visit(const NodeMin &n) override { binary(n, Op::Min); }
        void visit(const NodeMax &n) override { binary(n, Op::Max); }
        void visit(const NodeLog &n) override { unary(n, Op::Log); }
        void visit(const NodeExp &n) override { unary(n, Op::Exp); }
        void visit(const NodeSqrt &n) override { unary(n, Op::Sqrt); }
        void visit(const NodeAbs &n) override { unary(n, Op::Abs); }
        void visit(const NodeSmooth &n) override { binary(n, Op::Smooth); }

        // ── logic ───────────────────────────────────────────────────────────
        void visit(const NodeEqual &n) override { comparison(n, Op::Eq, Op::FCmpEq); }
        void visit(const NodeNotEqual &n) override { comparison(n, Op::Ne, Op::FCmpNe); }
        void visit(const NodeSuperior &n) override { comparison(n, Op::Gt, Op::FCmpGt); }
        void visit(const NodeSupEqual &n) override { comparison(n, Op::Ge, Op::FCmpGe); }
        void visit(const NodeInferior &n) override { comparison(n, Op::Lt, Op::FCmpLt); }
        void visit(const NodeInfEqual &n) override { comparison(n, Op::Le, Op::FCmpLe); }
        /// The regressors left to right (none: every spot of the event, as
        /// Evaluator::exercise), then the decision of this event.
        void visit(const NodeExercise &n) override
        {
            std::int32_t count = 0;
            if (n.arguments.empty())
                for (; count < n_spots_; ++count)
                {
                    max_spot_ = std::max(max_spot_, count);
                    emit(Op::Spot, count), push();
                }
            else
                for (const ExprTree &argument : n.arguments)
                {
                    argument->accept(*this);
                    ++count;
                }
            if (count > kMaxExerciseRegressors)
                throw InvalidInput("script compiler: exercise() / call() with more than " +
                                   std::to_string(kMaxExerciseRegressors) +
                                   " regressors runs on the tree evaluator only");
            p_.has_exercise = true;
            emit(Op::Exercise, count, event_, p_.fuzzy ? 1.0 : 0.0);
            for (std::int32_t i = 0; i < count; ++i)
                pop();
            if (p_.fuzzy)
                push_degree();
            else
                push();
        }
        void visit(const NodeAnd &n) override { connective(n, Op::And, Op::FAnd); }
        void visit(const NodeOr &n) override { connective(n, Op::Or, Op::FOr); }
        void visit(const NodeNot &n) override
        {
            n.arguments[0]->accept(*this);
            if (p_.fuzzy)
                emit(Op::FNot), pop_degree(), push_degree();
            else
                emit(Op::Not), pop(), push();
        }

        // ── statements ──────────────────────────────────────────────────────
        void visit(const NodeAssign &n) override
        {
            n.arguments[1]->accept(*this);
            const auto &var = static_cast<const NodeVar &>(*n.arguments[0]);
            emit(Op::Assign, static_cast<std::int32_t>(var.index)), pop();
        }
        void visit(const NodePays &n) override
        {
            n.arguments[0]->accept(*this);
            emit(Op::Pays), pop();
        }
        /// variables[c] = variables[c] + amount / numeraire, as the
        /// evaluator computes it.
        void visit(const NodeControlPays &n) override
        {
            const auto index = static_cast<std::int32_t>(static_cast<const NodeVar &>(*n.arguments[0]).index);
            emit(Op::Var, index), push();
            n.arguments[1]->accept(*this);
            emit(Op::Num), push();
            emit(Op::Div), pop(), pop(), push();
            emit(Op::Add), pop(), pop(), push();
            emit(Op::Assign, index), pop();
        }
        void visit(const NodeIf &n) override
        {
            const auto *cond = dynamic_cast<const NodeComparison *>(n.arguments[0].get());
            // A folded condition runs one branch and evaluates nothing, in
            // both evaluators.
            if (cond && cond->alwaysTrue)
                return statements(n, 1, n.firstElse);
            if (cond && cond->alwaysFalse)
                return statements(n, n.firstElse, n.arguments.size());

            n.arguments[0]->accept(*this);
            if (!p_.fuzzy)
            {
                const int jif = emit(Op::JumpIfFalse);
                pop();
                statements(n, 1, n.firstElse);
                const int jmp = emit(Op::Jump);
                p_.code[static_cast<std::size_t>(jif)].a = pc();
                statements(n, n.firstElse, n.arguments.size());
                p_.code[static_cast<std::size_t>(jmp)].a = pc();
                return;
            }

            const auto id = static_cast<std::int32_t>(p_.ifs.size());
            FuzzyIf f;
            f.first = static_cast<std::int32_t>(p_.aff.size());
            f.n = static_cast<std::int32_t>(n.affectedVars.size());
            // Nested ifs are live at the same time and get disjoint slots;
            // the counters rewind at the next event.
            f.slot = slot_;
            f.mode = mode_++;
            for (std::size_t slot : n.affectedVars)
                p_.aff.push_back(static_cast<std::int32_t>(slot));
            slot_ += 3 + 2 * f.n;
            p_.n_if_slots = std::max(p_.n_if_slots, slot_);
            p_.n_if_modes = std::max(p_.n_if_modes, mode_);
            p_.ifs.push_back(f);

            const int begin = emit(Op::FIfBegin, id);
            pop_degree();
            statements(n, 1, n.firstElse);
            const int mid = emit(Op::FIfMid, id);
            p_.code[static_cast<std::size_t>(begin)].b = pc();
            statements(n, n.firstElse, n.arguments.size());
            emit(Op::FIfEnd, id);
            p_.code[static_cast<std::size_t>(mid)].b = pc();
        }
        void visit(const NodeCollect &n) override { n.arguments[0]->accept(*this); }

      private:
        int pc() const { return static_cast<int>(p_.code.size()); }

        int emit(Op op, std::int32_t a = 0, std::int32_t b = 0, double x = 0.0)
        {
            p_.code.push_back(Instr{op, a, b, x});
            return pc() - 1;
        }

        void push() { p_.max_stack = std::max(p_.max_stack, ++sp_); }
        void pop()
        {
            if (--sp_ < 0)
                throw InvalidInput("script compiler: a number is read where only a condition was computed "
                                   "(in fuzzy mode, comparisons are degrees of truth, not numbers)");
        }
        void push_degree() { p_.max_degrees = std::max(p_.max_degrees, ++dp_); }
        void pop_degree()
        {
            if (--dp_ < 0)
                throw InvalidInput("script compiler: a condition is expected where a number was computed");
        }

        void binary(const Node &n, Op op)
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            emit(op), pop(), pop(), push();
        }
        void unary(const Node &n, Op op)
        {
            n.arguments[0]->accept(*this);
            emit(op), pop(), push();
        }
        void comparison(const NodeComparison &n, Op hard_op, Op fuzzy_op)
        {
            if (!p_.fuzzy)
            {
                // Evaluator::compare ignores the folding flags.
                n.arguments[0]->accept(*this);
                n.arguments[1]->accept(*this);
                emit(hard_op), pop(), pop(), push();
                return;
            }
            if (n.alwaysTrue || n.alwaysFalse)
            {
                emit(Op::FConst, 0, 0, n.alwaysTrue ? 1.0 : 0.0), push_degree();
                return;
            }
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            // a discrete condition interpolates between its domain's values
            // (NodeComparison::discrete_eps): the ramp on lhs - (rhs + shift)
            const bool interpolated = n.discrete && n.discrete_eps > 0.0;
            if (interpolated && n.discrete_shift != 0.0)
            {
                emit(Op::Const, 0, 0, n.discrete_shift), push();
                emit(Op::Add), pop(), pop(), push();
            }
            const bool crisp = !interpolated && (n.discrete || n.eps <= 0.0);
            const double eps = interpolated ? n.discrete_eps : n.eps;
            emit(fuzzy_op, crisp ? 1 : 0, smooth_ && !crisp ? 1 : 0, eps), pop(), pop(), push_degree();
        }
        void connective(const Node &n, Op hard_op, Op fuzzy_op)
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            if (p_.fuzzy)
                emit(fuzzy_op), pop_degree(), pop_degree(), push_degree();
            else
                emit(hard_op), pop(), pop(), push();
        }
        void statements(const NodeIf &n, std::size_t begin, std::size_t end)
        {
            for (std::size_t k = begin; k < end; ++k)
                n.arguments[k]->accept(*this);
        }

        Program p_;
        bool smooth_ = false;
        std::int32_t n_spots_ = 0;
        std::int32_t event_ = -1;
        int sp_ = 0;
        int dp_ = 0;
        int slot_ = 0;
        int mode_ = 0;
        std::int32_t max_spot_ = -1;
        std::int32_t max_df_ = -1;
        std::int32_t max_surv_ = -1;
        std::int32_t max_fwd_ = -1;
    };

    inline Program compile_script(const std::vector<Event> &events, std::size_t n_vars, bool fuzzy,
                                  bool smooth_ramp = false, std::size_t n_spots = 0)
    {
        return Compiler().compile(events, n_vars, fuzzy, smooth_ramp, n_spots);
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_COMPILER_HPP
