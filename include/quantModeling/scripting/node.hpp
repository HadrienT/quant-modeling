#ifndef QM_SCRIPTING_NODE_HPP
#define QM_SCRIPTING_NODE_HPP

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace quantModeling::scripting
{

    template <bool Const>
    class BasicVisitor;
    using ConstVisitor = BasicVisitor<true>;
    using Visitor = BasicVisitor<false>;

    struct Node;
    /// Sole ownership of an AST subtree — never shared_ptr (ADR-S4): a node has
    /// one parent and a lifetime that ends when the script is discarded.
    using ExprTree = std::unique_ptr<Node>;

    /**
     * @brief Base of every AST node.
     *
     * The hierarchy mirrors Andreasen & Savine's scripting book
     * (blueprint/wp/16-scripting.md §2). It is deliberately **not** templated on
     * a number type (ADR-S3): the tree describes structure, the Evaluator<T>
     * carries the numeric type, so one parse serves both a `double` price and
     * (with WP 17) a `Number` AAD pass.
     *
     * Children live in `arguments`, in evaluation order. Node-specific data is
     * scalar (values, indices, flags) and lives on the concrete type. Copy of a
     * Node copies only that scalar data — never the children; deep copies go
     * through clone().
     */
    struct Node
    {
        std::vector<ExprTree> arguments;

        Node() = default;
        virtual ~Node() = default;

        // A copied Node starts childless; clone() re-attaches deep-copied
        // children. This is what lets concrete nodes keep the implicitly
        // generated copy constructor despite holding a vector<unique_ptr>.
        Node(const Node &) {}
        Node &operator=(const Node &) { return *this; }
        Node(Node &&) = default;
        Node &operator=(Node &&) = default;

        virtual void accept(ConstVisitor &visitor) const = 0;
        virtual void accept(Visitor &visitor) = 0;

        /// Deep copy: same concrete type, same scalar data, cloned children.
        ExprTree clone() const
        {
            ExprTree copy = shallow_copy();
            copy->arguments.reserve(arguments.size());
            for (const ExprTree &child : arguments)
                copy->arguments.push_back(child ? child->clone() : nullptr);
            return copy;
        }

      protected:
        /// Concrete type, scalar data copied, no children.
        virtual ExprTree shallow_copy() const = 0;
    };

    /// CRTP glue: gives every concrete node its accept() and shallow_copy()
    /// without a hand-written override per type. `accept` is defined in
    /// visitor.hpp, once ConstVisitor is a complete type.
    template <class Derived>
    struct NodeT : Node
    {
        void accept(ConstVisitor &visitor) const override;
        void accept(Visitor &visitor) override;

      protected:
        ExprTree shallow_copy() const override
        {
            return std::make_unique<Derived>(static_cast<const Derived &>(*this));
        }
    };

    // ── leaves ────────────────────────────────────────────────────────────────

    struct NodeConst final : NodeT<NodeConst>
    {
        double value = 0.0;
    };

    /// A script variable. `index` is the slot in the evaluator's variable array;
    /// it is filled by the VarIndexer pass (WP 16b) and left at `unindexed`
    /// until then.
    struct NodeVar final : NodeT<NodeVar>
    {
        static constexpr std::size_t unindexed =
            std::numeric_limits<std::size_t>::max();

        std::string name;
        std::size_t index = unindexed;
    };

    /// The spot of one underlying at the current event date: `spot()` (asset
    /// 0, single-asset scripts) or `spot(i)` (asset i, i a non-negative
    /// integer literal -- multi-asset). `index` defaults to 0 so `spot()`
    /// keeps meaning exactly what it always did.
    struct NodeSpot final : NodeT<NodeSpot>
    {
        std::size_t index = 0;
    };

    // ── arithmetic ────────────────────────────────────────────────────────────

    struct NodeAdd final : NodeT<NodeAdd>
    {
    };
    struct NodeSub final : NodeT<NodeSub>
    {
    };
    struct NodeMult final : NodeT<NodeMult>
    {
    };
    struct NodeDiv final : NodeT<NodeDiv>
    {
    };
    struct NodePow final : NodeT<NodePow>
    {
    };
    struct NodeUplus final : NodeT<NodeUplus>
    {
    };
    struct NodeUminus final : NodeT<NodeUminus>
    {
    };

    // ── functions ─────────────────────────────────────────────────────────────

    struct NodeMin final : NodeT<NodeMin>
    {
    };
    struct NodeMax final : NodeT<NodeMax>
    {
    };
    struct NodeLog final : NodeT<NodeLog>
    {
    };
    struct NodeExp final : NodeT<NodeExp>
    {
    };
    struct NodeSqrt final : NodeT<NodeSqrt>
    {
    };
    struct NodeAbs final : NodeT<NodeAbs>
    {
    };
    /// Explicit smoothing request from the script: `smooth(x, halfwidth)`.
    struct NodeSmooth final : NodeT<NodeSmooth>
    {
    };

    // ── booleans ──────────────────────────────────────────────────────────────

    /// Base of the six comparison nodes. The flags are populated by the
    /// DomainProcessor / ConstCondProcessor passes (WP 16c) and unused in 16a.
    struct NodeComparison : Node
    {
        double eps = 0.0;         ///< smoothing half-width (DomainProcessor)
        bool discrete = false;    ///< operand takes only isolated values
        bool alwaysTrue = false;  ///< condition folds to true
        bool alwaysFalse = false; ///< condition folds to false
    };

    /// CRTP glue for comparison nodes: same as NodeT but rooted at
    /// NodeComparison so the shared flags survive a copy.
    template <class Derived>
    struct ComparisonT : NodeComparison
    {
        void accept(ConstVisitor &visitor) const override;
        void accept(Visitor &visitor) override;

      protected:
        ExprTree shallow_copy() const override
        {
            return std::make_unique<Derived>(static_cast<const Derived &>(*this));
        }
    };

    struct NodeEqual final : ComparisonT<NodeEqual>
    {
    };
    struct NodeNotEqual final : ComparisonT<NodeNotEqual>
    {
    };
    struct NodeSuperior final : ComparisonT<NodeSuperior>
    {
    };
    struct NodeSupEqual final : ComparisonT<NodeSupEqual>
    {
    };
    struct NodeInferior final : ComparisonT<NodeInferior>
    {
    };
    struct NodeInfEqual final : ComparisonT<NodeInfEqual>
    {
    };

    struct NodeAnd final : NodeT<NodeAnd>
    {
    };
    struct NodeOr final : NodeT<NodeOr>
    {
    };
    struct NodeNot final : NodeT<NodeNot>
    {
    };

    // ── statements ────────────────────────────────────────────────────────────

    /// `name = expr`. arguments[0] is the NodeVar, arguments[1] the expression.
    struct NodeAssign final : NodeT<NodeAssign>
    {
    };

    /// `pays expr`. arguments[0] is the amount; the evaluator deflates it by the
    /// numeraire of the current event date.
    struct NodePays final : NodeT<NodePays>
    {
    };

    /// `if cond then <stmts> [else <stmts>] endIf`.
    ///
    /// arguments[0] is the condition; arguments[1 .. firstElse) are the `then`
    /// statements; arguments[firstElse .. end) are the `else` statements.
    /// firstElse == arguments.size() when there is no `else`. `affectedVars` (the
    /// variable slots written by either branch) is filled by the IfProcessor
    /// pass (WP 16b) and empty in 16a.
    struct NodeIf final : NodeT<NodeIf>
    {
        std::size_t firstElse = 0;
        std::vector<std::size_t> affectedVars;
    };

    // ── root ──────────────────────────────────────────────────────────────────

    /// Top of a single parsed statement (Savine's "collect" node). arguments[0]
    /// is the statement (NodeAssign / NodePays / NodeIf). One per entry of
    /// Event::statements.
    struct NodeCollect final : NodeT<NodeCollect>
    {
    };

} // namespace quantModeling::scripting

// The CRTP accept() bodies need a complete ConstVisitor. visitor.hpp includes
// this header back; the include guards make the cycle safe, and pulling it in
// here means a plain `#include "scripting/node.hpp"` is enough to construct and
// visit nodes.
#include "quantModeling/scripting/visitor.hpp"

#endif // QM_SCRIPTING_NODE_HPP
