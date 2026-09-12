#ifndef QM_SCRIPTING_VISITORS_DEBUGGER_HPP
#define QM_SCRIPTING_VISITORS_DEBUGGER_HPP

#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <string>

namespace quantModeling::scripting
{

    /**
     * @brief Indented textual dump of an AST subtree — Savine's first test
     *        harness for the parser (blueprint/wp/16-scripting.md §4).
     *
     * One node per line, two spaces per level, most specific type name first,
     * then a leaf's literal (`Const 0.7`, `Var spot0`). Structural only: it
     * never prints the fields the later passes fill (NodeVar::index, a
     * comparison's eps / discrete), so two scripts with the same shape but
     * different surface syntax produce byte-identical dumps. That is the basis
     * of the round-trip property test.
     */
    class Debugger final : public ConstVisitor
    {
      public:
        /// Dump one statement / expression subtree.
        static std::string dump(const Node &root)
        {
            Debugger d;
            root.accept(d);
            return d.out_;
        }

        const std::string &result() const { return out_; }

        void visit(const NodeConst &n) override { leaf("Const " + number(n.value)); }
        void visit(const NodeVar &n) override { leaf("Var " + n.name); }
        // Unlike NodeVar::index (filled later, by VarIndexer), NodeSpot::index
        // comes straight from the surface syntax -- spot() vs spot(1) is as
        // much a difference in what was written as two different NodeConst
        // values, so it belongs in the dump the same way n.value does above.
        void visit(const NodeSpot &n) override
        {
            leaf("Spot " + std::to_string(n.index));
        }

        void visit(const NodeAdd &n) override { node("Add", n); }
        void visit(const NodeSub &n) override { node("Sub", n); }
        void visit(const NodeMult &n) override { node("Mult", n); }
        void visit(const NodeDiv &n) override { node("Div", n); }
        void visit(const NodePow &n) override { node("Pow", n); }
        void visit(const NodeUplus &n) override { node("Uplus", n); }
        void visit(const NodeUminus &n) override { node("Uminus", n); }

        void visit(const NodeMin &n) override { node("Min", n); }
        void visit(const NodeMax &n) override { node("Max", n); }
        void visit(const NodeLog &n) override { node("Log", n); }
        void visit(const NodeExp &n) override { node("Exp", n); }
        void visit(const NodeSqrt &n) override { node("Sqrt", n); }
        void visit(const NodeAbs &n) override { node("Abs", n); }
        void visit(const NodeSmooth &n) override { node("Smooth", n); }

        void visit(const NodeEqual &n) override { node("Equal", n); }
        void visit(const NodeNotEqual &n) override { node("NotEqual", n); }
        void visit(const NodeSuperior &n) override { node("Superior", n); }
        void visit(const NodeSupEqual &n) override { node("SupEqual", n); }
        void visit(const NodeInferior &n) override { node("Inferior", n); }
        void visit(const NodeInfEqual &n) override { node("InfEqual", n); }
        void visit(const NodeAnd &n) override { node("And", n); }
        void visit(const NodeOr &n) override { node("Or", n); }
        void visit(const NodeNot &n) override { node("Not", n); }

        void visit(const NodeAssign &n) override { node("Assign", n); }
        void visit(const NodePays &n) override { node("Pays", n); }
        void visit(const NodeIf &n) override
        {
            node("If firstElse=" + std::to_string(n.firstElse), n);
        }

        void visit(const NodeCollect &n) override { node("Collect", n); }

      private:
        void indent()
        {
            for (int i = 0; i < depth_; ++i)
                out_ += "  ";
        }

        void leaf(const std::string &label)
        {
            indent();
            out_ += label;
            out_ += '\n';
        }

        void node(const std::string &label, const Node &n)
        {
            leaf(label);
            ++depth_;
            for (const ExprTree &child : n.arguments)
                if (child)
                    child->accept(*this);
            --depth_;
        }

        /// A stable decimal spelling: trims trailing zeros so 0.70 and 0.7
        /// dump the same, keeps a leading digit.
        static std::string number(double value)
        {
            std::string s = std::to_string(value);
            if (s.find('.') != std::string::npos)
            {
                while (s.size() > 1 && s.back() == '0')
                    s.pop_back();
                if (!s.empty() && s.back() == '.')
                    s += '0';
            }
            return s;
        }

        std::string out_;
        int depth_ = 0;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DEBUGGER_HPP
