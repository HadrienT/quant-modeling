#ifndef QM_SCRIPTING_VISITOR_HPP
#define QM_SCRIPTING_VISITOR_HPP

#include "quantModeling/scripting/node.hpp"

namespace quantModeling::scripting
{

    /**
     * @brief Const visitor over the AST — the "cold path" dispatch.
     *
     * The pre-processing passes of WP 16b/16c (VarIndexer, DomainProcessor, …)
     * and the Debugger are ConstVisitors: a handful of calls per script, run
     * once at product construction, where a virtual call costs nothing and the
     * plain visitor reads best (ADR-S2). The hot per-path Evaluator will use a
     * separate CRTP mechanism, added in WP 16b.
     *
     * Every `visit` has a default that recurses into the node's children, so a
     * concrete visitor overrides only the nodes it cares about. Override without
     * calling visit_children() to stop the descent.
     */
    class ConstVisitor
    {
      public:
        virtual ~ConstVisitor() = default;

        virtual void visit(const NodeConst &n) { visit_children(n); }
        virtual void visit(const NodeVar &n) { visit_children(n); }
        virtual void visit(const NodeSpot &n) { visit_children(n); }

        virtual void visit(const NodeAdd &n) { visit_children(n); }
        virtual void visit(const NodeSub &n) { visit_children(n); }
        virtual void visit(const NodeMult &n) { visit_children(n); }
        virtual void visit(const NodeDiv &n) { visit_children(n); }
        virtual void visit(const NodePow &n) { visit_children(n); }
        virtual void visit(const NodeUplus &n) { visit_children(n); }
        virtual void visit(const NodeUminus &n) { visit_children(n); }

        virtual void visit(const NodeMin &n) { visit_children(n); }
        virtual void visit(const NodeMax &n) { visit_children(n); }
        virtual void visit(const NodeLog &n) { visit_children(n); }
        virtual void visit(const NodeExp &n) { visit_children(n); }
        virtual void visit(const NodeSqrt &n) { visit_children(n); }
        virtual void visit(const NodeAbs &n) { visit_children(n); }
        virtual void visit(const NodeSmooth &n) { visit_children(n); }

        virtual void visit(const NodeEqual &n) { visit_children(n); }
        virtual void visit(const NodeNotEqual &n) { visit_children(n); }
        virtual void visit(const NodeSuperior &n) { visit_children(n); }
        virtual void visit(const NodeSupEqual &n) { visit_children(n); }
        virtual void visit(const NodeInferior &n) { visit_children(n); }
        virtual void visit(const NodeInfEqual &n) { visit_children(n); }
        virtual void visit(const NodeAnd &n) { visit_children(n); }
        virtual void visit(const NodeOr &n) { visit_children(n); }
        virtual void visit(const NodeNot &n) { visit_children(n); }

        virtual void visit(const NodeAssign &n) { visit_children(n); }
        virtual void visit(const NodePays &n) { visit_children(n); }
        virtual void visit(const NodeIf &n) { visit_children(n); }

        virtual void visit(const NodeCollect &n) { visit_children(n); }

      protected:
        void visit_children(const Node &n)
        {
            for (const ExprTree &child : n.arguments)
                if (child)
                    child->accept(*this);
        }
    };

    // ── out-of-line CRTP accept(), now that ConstVisitor is complete ──────────

    template <class Derived>
    inline void NodeT<Derived>::accept(ConstVisitor &visitor) const
    {
        visitor.visit(static_cast<const Derived &>(*this));
    }

    template <class Derived>
    inline void ComparisonT<Derived>::accept(ConstVisitor &visitor) const
    {
        visitor.visit(static_cast<const Derived &>(*this));
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITOR_HPP
