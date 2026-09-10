#ifndef QM_SCRIPTING_VISITOR_HPP
#define QM_SCRIPTING_VISITOR_HPP

#include "quantModeling/scripting/node.hpp"

#include <type_traits>

namespace quantModeling::scripting
{

    /**
     * @brief Visitor over the AST — the "cold path" dispatch.
     *
     * `BasicVisitor<true>` (ConstVisitor) reads the tree: the Debugger, the
     * ScriptWriter, the hard Evaluator. `BasicVisitor<false>` (Visitor) mutates
     * it in place: the pre-processing passes (VarIndexer fills NodeVar::index,
     * IfProcessor fills NodeIf::affectedVars, …). These run once at product
     * construction, where a virtual call costs nothing and the plain visitor
     * reads best (ADR-S2). A hot per-path CRTP evaluator can be layered on later
     * without disturbing this.
     *
     * Every `visit` defaults to recursing into the node's children, so a
     * concrete visitor overrides only what it needs. Override without calling
     * visit_children() to stop the descent.
     */
    template <bool Const>
    class BasicVisitor
    {
        template <class N>
        using Ref = std::conditional_t<Const, const N &, N &>;

      public:
        virtual ~BasicVisitor() = default;

        virtual void visit(Ref<NodeConst> n) { visit_children(n); }
        virtual void visit(Ref<NodeVar> n) { visit_children(n); }
        virtual void visit(Ref<NodeSpot> n) { visit_children(n); }

        virtual void visit(Ref<NodeAdd> n) { visit_children(n); }
        virtual void visit(Ref<NodeSub> n) { visit_children(n); }
        virtual void visit(Ref<NodeMult> n) { visit_children(n); }
        virtual void visit(Ref<NodeDiv> n) { visit_children(n); }
        virtual void visit(Ref<NodePow> n) { visit_children(n); }
        virtual void visit(Ref<NodeUplus> n) { visit_children(n); }
        virtual void visit(Ref<NodeUminus> n) { visit_children(n); }

        virtual void visit(Ref<NodeMin> n) { visit_children(n); }
        virtual void visit(Ref<NodeMax> n) { visit_children(n); }
        virtual void visit(Ref<NodeLog> n) { visit_children(n); }
        virtual void visit(Ref<NodeExp> n) { visit_children(n); }
        virtual void visit(Ref<NodeSqrt> n) { visit_children(n); }
        virtual void visit(Ref<NodeAbs> n) { visit_children(n); }
        virtual void visit(Ref<NodeSmooth> n) { visit_children(n); }

        virtual void visit(Ref<NodeEqual> n) { visit_children(n); }
        virtual void visit(Ref<NodeNotEqual> n) { visit_children(n); }
        virtual void visit(Ref<NodeSuperior> n) { visit_children(n); }
        virtual void visit(Ref<NodeSupEqual> n) { visit_children(n); }
        virtual void visit(Ref<NodeInferior> n) { visit_children(n); }
        virtual void visit(Ref<NodeInfEqual> n) { visit_children(n); }
        virtual void visit(Ref<NodeAnd> n) { visit_children(n); }
        virtual void visit(Ref<NodeOr> n) { visit_children(n); }
        virtual void visit(Ref<NodeNot> n) { visit_children(n); }

        virtual void visit(Ref<NodeAssign> n) { visit_children(n); }
        virtual void visit(Ref<NodePays> n) { visit_children(n); }
        virtual void visit(Ref<NodeIf> n) { visit_children(n); }

        virtual void visit(Ref<NodeCollect> n) { visit_children(n); }

      protected:
        void visit_children(Ref<Node> n)
        {
            for (Ref<ExprTree> child : n.arguments)
                if (child)
                    child->accept(*this);
        }
    };

    using ConstVisitor = BasicVisitor<true>;
    using Visitor = BasicVisitor<false>;

    // ── out-of-line CRTP accept(), now that the visitors are complete ─────────

    template <class Derived>
    inline void NodeT<Derived>::accept(ConstVisitor &visitor) const
    {
        visitor.visit(static_cast<const Derived &>(*this));
    }

    template <class Derived>
    inline void NodeT<Derived>::accept(Visitor &visitor)
    {
        visitor.visit(static_cast<Derived &>(*this));
    }

    template <class Derived>
    inline void ComparisonT<Derived>::accept(ConstVisitor &visitor) const
    {
        visitor.visit(static_cast<const Derived &>(*this));
    }

    template <class Derived>
    inline void ComparisonT<Derived>::accept(Visitor &visitor)
    {
        visitor.visit(static_cast<Derived &>(*this));
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITOR_HPP
