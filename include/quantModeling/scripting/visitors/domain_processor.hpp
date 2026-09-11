#ifndef QM_SCRIPTING_VISITORS_DOMAIN_PROCESSOR_HPP
#define QM_SCRIPTING_VISITORS_DOMAIN_PROCESSOR_HPP

#include "quantModeling/scripting/domain.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <cstddef>
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
                return;
            }
            n.eps = default_eps_;
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
