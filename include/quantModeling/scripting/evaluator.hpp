#ifndef QM_SCRIPTING_EVALUATOR_HPP
#define QM_SCRIPTING_EVALUATOR_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Hard (crisp) evaluator of a scripted product on one path
     *        (blueprint/wp/16-scripting.md §5.1).
     *
     * A ConstVisitor with an explicit value stack: expression nodes push, the
     * three statement nodes act. Variables live in `variables_` (sized by the
     * VarIndexer) and persist across the events of a path; `pays` accumulates
     * `amount / numeraire(event)` into `payoff_`, so the result is already
     * deflated, as ISimulatableProduct::payoffs() requires.
     *
     * Templated on the number type: T = Real prices, and (with WP 17) an AAD
     * `Number` differentiates the very same tree. All maths goes through
     * unqualified calls after `using std::…` so ADL picks the AAD overloads
     * when T = Number (ADR-A/§5.5). Control flow is not differentiated — a hard
     * `if` picks a branch; smoothing is the job of FuzzyEvaluator (WP 16c).
     *
     * Not thread-safe: one Evaluator per thread, the AST shared const (§5.4).
     */
    template <class T = Real>
    class Evaluator final : public ConstVisitor
    {
      public:
        void set_variable_count(std::size_t n) { variables_.assign(n, T(0)); }

        /// Reset for a new path. Call once, then run every event in order.
        void initialize()
        {
            std::fill(variables_.begin(), variables_.end(), T(0));
            payoff_ = T(0);
            stack_.clear();
        }

        void set_event(const Scenario<T> &scenario, std::size_t event_index)
        {
            scenario_ = &scenario;
            event_index_ = event_index;
        }

        void run(const Node &statement) { statement.accept(*this); }

        const T &payoff() const { return payoff_; }
        const std::vector<T> &variables() const { return variables_; }

        // ── leaves ──────────────────────────────────────────────────────────
        void visit(const NodeConst &n) override { push(T(n.value)); }
        void visit(const NodeVar &n) override { push(variables_[n.index]); }
        void visit(const NodeSpot &) override
        {
            push((*scenario_)[event_index_].spots[0]);
        }

        // ── arithmetic ──────────────────────────────────────────────────────
        void visit(const NodeAdd &n) override
        {
            binary(n, [](const T &a, const T &b) { return a + b; });
        }
        void visit(const NodeSub &n) override
        {
            binary(n, [](const T &a, const T &b) { return a - b; });
        }
        void visit(const NodeMult &n) override
        {
            binary(n, [](const T &a, const T &b) { return a * b; });
        }
        void visit(const NodeDiv &n) override
        {
            binary(n, [](const T &a, const T &b) { return a / b; });
        }
        void visit(const NodePow &n) override
        {
            binary(n, [](const T &a, const T &b)
                   { using std::pow; return pow(a, b); });
        }
        void visit(const NodeUplus &n) override
        {
            unary(n, [](const T &a) { return a; });
        }
        void visit(const NodeUminus &n) override
        {
            unary(n, [](const T &a) { return -a; });
        }

        // ── functions ───────────────────────────────────────────────────────
        void visit(const NodeMin &n) override
        {
            binary(n, [](const T &a, const T &b)
                   { using std::min; return min(a, b); });
        }
        void visit(const NodeMax &n) override
        {
            binary(n, [](const T &a, const T &b)
                   { using std::max; return max(a, b); });
        }
        void visit(const NodeLog &n) override
        {
            unary(n, [](const T &a) { using std::log; return log(a); });
        }
        void visit(const NodeExp &n) override
        {
            unary(n, [](const T &a) { using std::exp; return exp(a); });
        }
        void visit(const NodeSqrt &n) override
        {
            unary(n, [](const T &a) { using std::sqrt; return sqrt(a); });
        }
        void visit(const NodeAbs &n) override
        {
            unary(n, [](const T &a) { using std::fabs; return fabs(a); });
        }
        void visit(const NodeSmooth &n) override
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T h = pop();
            const T x = pop();
            const double hd = static_cast<double>(h);
            if (hd <= 0.0)
            {
                push(static_cast<double>(x) > 0.0 ? T(1) : T(0));
                return;
            }
            T ramp = (x + h) / (h + h); // (x + h) / (2h)
            const double r = static_cast<double>(ramp);
            if (r <= 0.0)
                push(T(0));
            else if (r >= 1.0)
                push(T(1));
            else
                push(std::move(ramp));
        }

        // ── comparisons and connectives → 1 / 0 ────────────────────────────
        void visit(const NodeEqual &n) override
        {
            compare(n, [](const T &a, const T &b) { return a == b; });
        }
        void visit(const NodeNotEqual &n) override
        {
            compare(n, [](const T &a, const T &b) { return a != b; });
        }
        void visit(const NodeSuperior &n) override
        {
            compare(n, [](const T &a, const T &b) { return a > b; });
        }
        void visit(const NodeSupEqual &n) override
        {
            compare(n, [](const T &a, const T &b) { return a >= b; });
        }
        void visit(const NodeInferior &n) override
        {
            compare(n, [](const T &a, const T &b) { return a < b; });
        }
        void visit(const NodeInfEqual &n) override
        {
            compare(n, [](const T &a, const T &b) { return a <= b; });
        }
        void visit(const NodeAnd &n) override
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop();
            const T a = pop();
            push(truthy(a) && truthy(b) ? T(1) : T(0));
        }
        void visit(const NodeOr &n) override
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop();
            const T a = pop();
            push(truthy(a) || truthy(b) ? T(1) : T(0));
        }
        void visit(const NodeNot &n) override
        {
            n.arguments[0]->accept(*this);
            push(truthy(pop()) ? T(0) : T(1));
        }

        // ── statements ──────────────────────────────────────────────────────
        void visit(const NodeAssign &n) override
        {
            n.arguments[1]->accept(*this);
            const auto &var = static_cast<const NodeVar &>(*n.arguments[0]);
            variables_[var.index] = pop();
        }
        void visit(const NodePays &n) override
        {
            n.arguments[0]->accept(*this);
            payoff_ += pop() / (*scenario_)[event_index_].numeraire;
        }
        void visit(const NodeIf &n) override
        {
            const auto *cond =
                dynamic_cast<const NodeComparison *>(n.arguments[0].get());
            bool take_then;
            if (cond && cond->alwaysTrue)
                take_then = true;
            else if (cond && cond->alwaysFalse)
                take_then = false;
            else
            {
                n.arguments[0]->accept(*this);
                take_then = truthy(pop());
            }

            const std::size_t begin = take_then ? 1 : n.firstElse;
            const std::size_t end = take_then ? n.firstElse : n.arguments.size();
            for (std::size_t k = begin; k < end; ++k)
                n.arguments[k]->accept(*this);
        }
        void visit(const NodeCollect &n) override
        {
            n.arguments[0]->accept(*this);
        }

      private:
        void push(T value) { stack_.push_back(std::move(value)); }
        T pop()
        {
            T value = std::move(stack_.back());
            stack_.pop_back();
            return value;
        }

        template <class F>
        void binary(const Node &n, F f)
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop();
            const T a = pop();
            push(f(a, b));
        }
        template <class F>
        void unary(const Node &n, F f)
        {
            n.arguments[0]->accept(*this);
            const T a = pop();
            push(f(a));
        }
        template <class P>
        void compare(const Node &n, P p)
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop();
            const T a = pop();
            push(p(a, b) ? T(1) : T(0));
        }
        static bool truthy(const T &v) { return static_cast<double>(v) != 0.0; }

        std::vector<T> variables_;
        std::vector<T> stack_;
        const Scenario<T> *scenario_ = nullptr;
        std::size_t event_index_ = 0;
        T payoff_{};
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_EVALUATOR_HPP
