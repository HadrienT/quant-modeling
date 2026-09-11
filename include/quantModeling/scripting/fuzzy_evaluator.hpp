#ifndef QM_SCRIPTING_FUZZY_EVALUATOR_HPP
#define QM_SCRIPTING_FUZZY_EVALUATOR_HPP

#include "quantModeling/scripting/evaluator.hpp"
#include "quantModeling/scripting/node.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Fuzzy-logic evaluator: a payoff continuous, and differentiable
     *        almost everywhere, in the path (blueprint/wp/16-scripting.md §5.2).
     *
     * Every non-discrete comparison becomes a **degree of truth** in [0,1] via
     * a call spread of half-width `eps` (set by the DomainProcessor); `and` /
     * `or` / `not` are `min` / `max` / `1−·`. An `if` with a strictly interior
     * degree `dt` runs **both** branches and blends them,
     * `v = dt·v_then + (1−dt)·v_else`, on the variables either branch writes
     * (`NodeIf::affectedVars`) and on the accumulated payoff. On the common
     * paths, far from a boundary, `dt` is 0 or 1 and only one branch runs — no
     * overhead. Discrete tests (a `ki = 1` flag) stay crisp.
     *
     * The degree stack is `T` too, so with T = Number (WP 17) the blend carries
     * the derivative that gives a correct pathwise delta on a digital or a
     * barrier — the reason this evaluator exists.
     */
    template <class T = Real>
    class FuzzyEvaluator final : public Evaluator<T>
    {
        using Base = Evaluator<T>;

      public:
        void initialize() override
        {
            Base::initialize();
            degrees_.clear();
        }

        void visit(const NodeEqual &n) override { comparison(n, Cmp::Eq); }
        void visit(const NodeNotEqual &n) override { comparison(n, Cmp::Ne); }
        void visit(const NodeSuperior &n) override { comparison(n, Cmp::Gt); }
        void visit(const NodeSupEqual &n) override { comparison(n, Cmp::Ge); }
        void visit(const NodeInferior &n) override { comparison(n, Cmp::Lt); }
        void visit(const NodeInfEqual &n) override { comparison(n, Cmp::Le); }

        void visit(const NodeAnd &n) override
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop_degree();
            const T a = pop_degree();
            using std::min;
            push_degree(min(a, b));
        }
        void visit(const NodeOr &n) override
        {
            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T b = pop_degree();
            const T a = pop_degree();
            using std::max;
            push_degree(max(a, b));
        }
        void visit(const NodeNot &n) override
        {
            n.arguments[0]->accept(*this);
            push_degree(T(1) - pop_degree());
        }

        void visit(const NodeIf &n) override
        {
            const T degree = condition_degree(n);
            const double dt = static_cast<double>(degree);

            if (dt <= 0.0)
            {
                run(n, n.firstElse, n.arguments.size());
                return;
            }
            if (dt >= 1.0)
            {
                run(n, 1, n.firstElse);
                return;
            }

            // strictly interior degree → evaluate both branches and blend
            const T payoff0 = this->payoff_;
            std::vector<T> saved;
            saved.reserve(n.affectedVars.size());
            for (std::size_t slot : n.affectedVars)
                saved.push_back(this->variables_[slot]);

            run(n, 1, n.firstElse);
            std::vector<T> then_vars;
            then_vars.reserve(n.affectedVars.size());
            for (std::size_t slot : n.affectedVars)
                then_vars.push_back(this->variables_[slot]);
            const T payoff_then = this->payoff_;

            for (std::size_t i = 0; i < n.affectedVars.size(); ++i)
                this->variables_[n.affectedVars[i]] = saved[i];
            this->payoff_ = payoff0;
            run(n, n.firstElse, n.arguments.size());
            const T payoff_else = this->payoff_;

            const T w1 = T(1) - degree;
            for (std::size_t i = 0; i < n.affectedVars.size(); ++i)
            {
                const std::size_t slot = n.affectedVars[i];
                this->variables_[slot] =
                    degree * then_vars[i] + w1 * this->variables_[slot];
            }
            this->payoff_ = payoff0 + degree * (payoff_then - payoff0) +
                            w1 * (payoff_else - payoff0);
        }

      private:
        enum class Cmp
        {
            Eq,
            Ne,
            Gt,
            Ge,
            Lt,
            Le
        };

        void push_degree(T d) { degrees_.push_back(std::move(d)); }
        T pop_degree()
        {
            T d = std::move(degrees_.back());
            degrees_.pop_back();
            return d;
        }

        void run(const NodeIf &n, std::size_t begin, std::size_t end)
        {
            for (std::size_t k = begin; k < end; ++k)
                n.arguments[k]->accept(*this);
        }

        T condition_degree(const NodeIf &n)
        {
            const auto *cmp =
                dynamic_cast<const NodeComparison *>(n.arguments[0].get());
            if (cmp && cmp->alwaysTrue)
                return T(1);
            if (cmp && cmp->alwaysFalse)
                return T(0);
            n.arguments[0]->accept(*this);
            return pop_degree();
        }

        void comparison(const NodeComparison &n, Cmp c)
        {
            if (n.alwaysTrue)
            {
                push_degree(T(1));
                return;
            }
            if (n.alwaysFalse)
            {
                push_degree(T(0));
                return;
            }

            n.arguments[0]->accept(*this);
            n.arguments[1]->accept(*this);
            const T rhs = this->pop();
            const T lhs = this->pop();
            const T x = lhs - rhs; // condition is x ⋈ 0

            if (n.discrete || n.eps <= 0.0)
            {
                push_degree(hard(static_cast<double>(x), c) ? T(1) : T(0));
                return;
            }
            push_degree(call_spread(x, n.eps, c));
        }

        static bool hard(double x, Cmp c)
        {
            switch (c)
            {
                case Cmp::Eq:
                    return x == 0.0;
                case Cmp::Ne:
                    return x != 0.0;
                case Cmp::Gt:
                    return x > 0.0;
                case Cmp::Ge:
                    return x >= 0.0;
                case Cmp::Lt:
                    return x < 0.0;
                case Cmp::Le:
                    return x <= 0.0;
            }
            return false;
        }

        /// Degree that `x ⋈ 0` holds, smoothed over a band of half-width eps.
        static T call_spread(const T &x, double eps, Cmp c)
        {
            const T up = clamp01((x + T(eps)) / T(2.0 * eps)); // degree of x > 0
            switch (c)
            {
                case Cmp::Gt:
                case Cmp::Ge:
                    return up;
                case Cmp::Lt:
                case Cmp::Le:
                    return T(1) - up;
                case Cmp::Eq:
                    return tent(x, eps);
                case Cmp::Ne:
                    return T(1) - tent(x, eps);
            }
            return up;
        }

        /// max(0, 1 − |x|/eps): 1 at x = 0, 0 beyond ±eps.
        static T tent(const T &x, double eps)
        {
            using std::fabs;
            return clamp01(T(1) - fabs(x) / T(eps));
        }

        static T clamp01(T v)
        {
            const double d = static_cast<double>(v);
            if (d <= 0.0)
                return T(0);
            if (d >= 1.0)
                return T(1);
            return v;
        }

        std::vector<T> degrees_;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_FUZZY_EVALUATOR_HPP
