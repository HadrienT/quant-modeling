#ifndef QM_SCRIPTING_VISITORS_CONST_COND_HPP
#define QM_SCRIPTING_VISITORS_CONST_COND_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <cmath>
#include <optional>
#include <vector>

namespace quantModeling::scripting
{

    /// Value of a purely constant expression subtree (NodeConst through
    /// arithmetic and the deterministic functions), or nullopt if it depends on
    /// a variable or the market.
    inline std::optional<double> try_eval_const(const Node &node)
    {
        auto arg = [&](std::size_t k)
        { return try_eval_const(*node.arguments[k]); };

        if (const auto *c = dynamic_cast<const NodeConst *>(&node))
            return c->value;
        if (dynamic_cast<const NodeVar *>(&node) ||
            dynamic_cast<const NodeSpot *>(&node))
            return std::nullopt;

        if (dynamic_cast<const NodeAdd *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(*a + *b) : std::nullopt;
        }
        if (dynamic_cast<const NodeSub *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(*a - *b) : std::nullopt;
        }
        if (dynamic_cast<const NodeMult *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(*a * *b) : std::nullopt;
        }
        if (dynamic_cast<const NodeDiv *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b && *b != 0.0) ? std::optional(*a / *b) : std::nullopt;
        }
        if (dynamic_cast<const NodePow *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(std::pow(*a, *b)) : std::nullopt;
        }
        if (dynamic_cast<const NodeUminus *>(&node))
        {
            auto a = arg(0);
            return a ? std::optional(-*a) : std::nullopt;
        }
        if (dynamic_cast<const NodeUplus *>(&node))
            return arg(0);
        if (dynamic_cast<const NodeMin *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(std::min(*a, *b)) : std::nullopt;
        }
        if (dynamic_cast<const NodeMax *>(&node))
        {
            auto a = arg(0), b = arg(1);
            return (a && b) ? std::optional(std::max(*a, *b)) : std::nullopt;
        }
        if (dynamic_cast<const NodeLog *>(&node))
        {
            auto a = arg(0);
            return a ? std::optional(std::log(*a)) : std::nullopt;
        }
        if (dynamic_cast<const NodeExp *>(&node))
        {
            auto a = arg(0);
            return a ? std::optional(std::exp(*a)) : std::nullopt;
        }
        if (dynamic_cast<const NodeSqrt *>(&node))
        {
            auto a = arg(0);
            return a ? std::optional(std::sqrt(*a)) : std::nullopt;
        }
        if (dynamic_cast<const NodeAbs *>(&node))
        {
            auto a = arg(0);
            return a ? std::optional(std::fabs(*a)) : std::nullopt;
        }
        return std::nullopt;
    }

    /**
     * @brief Fold conditions whose truth is fixed at parse time.
     *
     * When both operands of a comparison are constant, its result is known:
     * NodeComparison::alwaysTrue / alwaysFalse is set. The 16c DomainProcessor
     * generalises this to domain-based folding and drives the smoothing
     * decision; here it is only the free, purely-constant case
     * (blueprint/wp/16-scripting.md §4).
     */
    class ConstCondProcessor final : public Visitor
    {
      public:
        void process(std::vector<Event> &events)
        {
            for (Event &event : events)
                for (ExprTree &statement : event.statements)
                    statement->accept(*this);
        }

        void visit(NodeEqual &n) override
        {
            fold(n, [](double a, double b)
                 { return a == b; });
        }
        void visit(NodeNotEqual &n) override
        {
            fold(n, [](double a, double b)
                 { return a != b; });
        }
        void visit(NodeSuperior &n) override
        {
            fold(n, [](double a, double b)
                 { return a > b; });
        }
        void visit(NodeSupEqual &n) override
        {
            fold(n, [](double a, double b)
                 { return a >= b; });
        }
        void visit(NodeInferior &n) override
        {
            fold(n, [](double a, double b)
                 { return a < b; });
        }
        void visit(NodeInfEqual &n) override
        {
            fold(n, [](double a, double b)
                 { return a <= b; });
        }

      private:
        template <class Op>
        void fold(NodeComparison &n, Op op)
        {
            visit_children(n);
            const auto a = try_eval_const(*n.arguments[0]);
            const auto b = try_eval_const(*n.arguments[1]);
            if (a && b)
            {
                n.alwaysTrue = op(*a, *b);
                n.alwaysFalse = !n.alwaysTrue;
            }
        }
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_CONST_COND_HPP
