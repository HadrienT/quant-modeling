#ifndef QM_SCRIPTING_VISITORS_DOMAIN_PROCESSOR_HPP
#define QM_SCRIPTING_VISITORS_DOMAIN_PROCESSOR_HPP

#include "quantModeling/scripting/domain.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <cstddef>
#include <cmath>
#include <limits>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Propagate value domains and, from them, mark every condition
     *        (blueprint/wp/16-scripting.md §4.2).
     *
     * Two walks over the script:
     *  1. collect each variable's domain — the union of every right-hand side
     *     ever assigned to it (an unassigned variable is `{0}`, matching the
     *     evaluator's zero-initialisation);
     *  2. for each comparison `lhs ⋈ rhs`, take the domain of `lhs - rhs` and
     *     set on the node: `discrete` (finite isolated domain — a flag test,
     *     never smoothed), `alwaysTrue` / `alwaysFalse` (domain wholly on one
     *     side of zero), or `eps` (the smoothing half-width).
     *
     * The collection is flow-insensitive: a condition sees the union over every
     * assignment, not only those on paths that reach it. This over-approximates
     * — it may smooth where a sharper analysis would fold, never the reverse
     * (ADR-S6, to reconcile with the book's flow-sensitive pass).
     */
    class DomainProcessor final : public Visitor
    {
      public:
        DomainProcessor(std::size_t n_vars, double default_eps)
            : var_domain_(n_vars, Domain::singleton(0.0)),
              default_eps_(default_eps)
        {
        }

        void process(std::vector<Event> &events)
        {
            for (Event &event : events)
                for (ExprTree &statement : event.statements)
                    collect(*statement);
            for (Event &event : events)
                for (ExprTree &statement : event.statements)
                    statement->accept(*this);
        }

        const Domain &variable_domain(std::size_t slot) const
        {
            return var_domain_[slot];
        }

        void visit(NodeEqual &n) override { mark(n, Op::Eq); }
        void visit(NodeNotEqual &n) override { mark(n, Op::Ne); }
        void visit(NodeSuperior &n) override { mark(n, Op::Gt); }
        void visit(NodeSupEqual &n) override { mark(n, Op::Ge); }
        void visit(NodeInferior &n) override { mark(n, Op::Lt); }
        void visit(NodeInfEqual &n) override { mark(n, Op::Le); }

      private:
        enum class Op
        {
            Eq,
            Ne,
            Gt,
            Ge,
            Lt,
            Le
        };

        void collect(const Node &node)
        {
            if (const auto *assign = dynamic_cast<const NodeAssign *>(&node))
            {
                const auto &var =
                    static_cast<const NodeVar &>(*assign->arguments[0]);
                var_domain_[var.index] = domain_union(
                    var_domain_[var.index], domain_of(*assign->arguments[1]));
                return;
            }
            if (const auto *control = dynamic_cast<const NodeControlPays *>(&node))
            {
                // A running sum of deflated amounts: no script reads it.
                const auto &var = static_cast<const NodeVar &>(*control->arguments[0]);
                var_domain_[var.index] = Domain::real_line();
                return;
            }
            for (const ExprTree &child : node.arguments)
                if (child)
                    collect(*child);
        }

        Domain domain_of(const Node &n) const
        {
            auto d = [&](std::size_t k)
            { return domain_of(*n.arguments[k]); };

            if (const auto *c = dynamic_cast<const NodeConst *>(&n))
                return Domain::singleton(c->value);
            if (const auto *v = dynamic_cast<const NodeVar *>(&n))
                return var_domain_[v->index];
            if (dynamic_cast<const NodeSpot *>(&n))
                return Domain::greater_than(0.0);
            if (dynamic_cast<const NodeDf *>(&n) || dynamic_cast<const NodeFwd *>(&n) ||
                dynamic_cast<const NodeYf *>(&n))
                return Domain::greater_than(0.0);
            if (const auto *s = dynamic_cast<const NodeStepQuantity *>(&n))
                switch (s->kind)
                {
                    case StepQuantity::LastStepCall:
                        return Domain::at_least(0.0);
                    case StepQuantity::LastStepPut: // below its strike: the script's own, if it computes it
                        return s->computed_strike() ? Domain::at_least(0.0) : Domain::closed(0.0, s->level);
                    default: // a probability
                        return Domain::closed(0.0, 1.0);
                }

            if (dynamic_cast<const NodeAdd *>(&n))
                return d(0) + d(1);
            if (dynamic_cast<const NodeSub *>(&n))
                return d(0) - d(1);
            if (dynamic_cast<const NodeMult *>(&n))
                return d(0) * d(1);
            if (dynamic_cast<const NodeDiv *>(&n))
                return d(0) / d(1);
            if (dynamic_cast<const NodeUminus *>(&n))
                return -d(0);
            if (dynamic_cast<const NodeUplus *>(&n))
                return d(0);
            if (dynamic_cast<const NodeMin *>(&n))
                return domain_min(d(0), d(1));
            if (dynamic_cast<const NodeMax *>(&n))
                return domain_max(d(0), d(1));
            if (dynamic_cast<const NodeAbs *>(&n))
                return domain_abs(d(0));
            if (dynamic_cast<const NodeExp *>(&n))
                return domain_exp(d(0));
            if (dynamic_cast<const NodeLog *>(&n))
                return domain_log(d(0));
            if (dynamic_cast<const NodeSqrt *>(&n))
                return domain_sqrt(d(0));
            if (dynamic_cast<const NodeSmooth *>(&n))
                return Domain::closed(0.0, 1.0);
            if (dynamic_cast<const NodeExercise *>(&n))
                return domain_union(Domain::singleton(0.0), Domain::singleton(1.0));

            return Domain::real_line(); // NodePow and anything unmodelled
        }

        void mark(NodeComparison &n, Op op)
        {
            const Domain diff =
                domain_of(*n.arguments[0]) - domain_of(*n.arguments[1]);

            // region of `diff` on which the comparison is true
            bool always_true = false;
            bool always_false = false;
            switch (op)
            {
                case Op::Gt:
                    always_true = diff.all_positive();
                    always_false = diff.all_non_positive();
                    break;
                case Op::Ge:
                    always_true = diff.all_non_negative();
                    always_false = diff.all_negative();
                    break;
                case Op::Lt:
                    always_true = diff.all_negative();
                    always_false = diff.all_non_negative();
                    break;
                case Op::Le:
                    always_true = diff.all_non_positive();
                    always_false = diff.all_positive();
                    break;
                case Op::Eq:
                    always_true = is_exactly_zero(diff);
                    always_false = !diff.empty() && !diff.contains(0.0);
                    break;
                case Op::Ne:
                    always_true = !diff.empty() && !diff.contains(0.0);
                    always_false = is_exactly_zero(diff);
                    break;
            }

            if (always_true)
            {
                n.alwaysTrue = true;
                return;
            }
            if (always_false)
            {
                n.alwaysFalse = true;
                return;
            }
            if (diff.is_discrete())
            {
                n.discrete = true; // isolated values straddling the boundary
                discrete_band(n, op, diff);
                return;
            }
            n.eps = default_eps_;
        }

        /// The fuzzy mode's interpolation of a discrete condition (WP 26,
        /// #226 follow-up; a departure from the book, which keeps it hard):
        /// a fuzzy `if` blends the variables it assigns, so a 0 / 1 flag can
        /// hold 0.97, which a hard `flag = 1` reads as false -- the payoff of
        /// every path near a barrier then vanishes. The ramp interpolates
        /// between the domain's neighbours of the boundary: for x > 0 (and
        /// its negation x <= 0), a = the largest value <= 0, b = the smallest
        /// > 0; for x >= 0 (and x < 0), a = the largest < 0, b = the smallest
        /// >= 0; for x = 0 (and x != 0), a tent of the half-width of the
        /// nearest non-zero value. On the domain's own values the degree is
        /// the hard result, so hard and fuzzy agree wherever a flag is
        /// crisp.
        static void discrete_band(NodeComparison &n, Op op, const Domain &diff)
        {
            std::vector<double> pts;
            for (const Domain::Interval &p : diff.parts())
                pts.push_back(p.lo);
            const double inf = std::numeric_limits<double>::infinity();
            if (op == Op::Eq || op == Op::Ne)
            {
                double g = inf;
                for (double d : pts)
                    if (d != 0.0)
                        g = std::min(g, std::fabs(d));
                if (g < inf)
                {
                    n.discrete_eps = g;
                    n.discrete_shift = 0.0;
                }
                return;
            }
            const bool positive_side_strict = op == Op::Gt || op == Op::Le; // x > 0 is the "true" side
            double a = -inf, b = inf;
            for (double d : pts)
            {
                const bool upper = positive_side_strict ? d > 0.0 : d >= 0.0;
                if (upper)
                    b = std::min(b, d);
                else
                    a = std::max(a, d);
            }
            if (a > -inf && b < inf)
            {
                n.discrete_eps = 0.5 * (b - a);
                n.discrete_shift = 0.5 * (a + b);
            }
        }

        static bool is_exactly_zero(const Domain &d)
        {
            return d.parts().size() == 1 && d.parts().front().is_singleton() &&
                   d.parts().front().lo == 0.0;
        }

        std::vector<Domain> var_domain_;
        double default_eps_;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DOMAIN_PROCESSOR_HPP
