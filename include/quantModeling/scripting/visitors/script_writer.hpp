#ifndef QM_SCRIPTING_VISITORS_SCRIPT_WRITER_HPP
#define QM_SCRIPTING_VISITORS_SCRIPT_WRITER_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <string>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Re-emit a parsed script as canonical text.
     *
     * Every binary operation is fully parenthesised and every block is
     * re-indented, so the output is unambiguous and re-parses to the same AST.
     * The round-trip property test checks
     * `dump(parse(s)) == dump(parse(write(parse(s))))` — the tree is a fixed
     * point of `parse ∘ write` (blueprint/wp/16-scripting.md §9). It is not a
     * source-formatter: it does not try to reproduce the original layout.
     */
    class ScriptWriter final
    {
      public:
        static std::string write(const std::vector<Event> &events)
        {
            std::string out;
            for (std::size_t e = 0; e < events.size(); ++e)
            {
                if (e > 0)
                    out += '\n';
                out += events[e].date.to_iso();
                out += '\n';
                for (const ExprTree &stmt : events[e].statements)
                    write_statement(*stmt, 1, out);
            }
            return out;
        }

      private:
        static std::string pad(int depth) { return std::string(depth * 4, ' '); }

        /// `stmt` is a NodeCollect wrapping one Assign / Pays / If.
        static void write_statement(const Node &stmt, int depth, std::string &out)
        {
            const Node &inner = *stmt.arguments.front();

            if (const auto *a = dynamic_cast<const NodeAssign *>(&inner))
            {
                const auto *var =
                    dynamic_cast<const NodeVar *>(a->arguments[0].get());
                out += pad(depth) + var->name + " = " +
                       expr(*a->arguments[1]) + '\n';
                return;
            }
            if (const auto *p = dynamic_cast<const NodePays *>(&inner))
            {
                out += pad(depth) + "pays " + expr(*p->arguments[0]) + '\n';
                return;
            }
            if (const auto *iff = dynamic_cast<const NodeIf *>(&inner))
            {
                out += pad(depth) + "if " + expr(*iff->arguments[0]) + " then\n";
                for (std::size_t k = 1; k < iff->firstElse; ++k)
                    write_statement(*iff->arguments[k], depth + 1, out);
                if (iff->firstElse < iff->arguments.size())
                {
                    out += pad(depth) + "else\n";
                    for (std::size_t k = iff->firstElse;
                         k < iff->arguments.size(); ++k)
                        write_statement(*iff->arguments[k], depth + 1, out);
                }
                out += pad(depth) + "endIf\n";
                return;
            }
        }

        static std::string expr(const Node &n)
        {
            Printer p;
            n.accept(p);
            return p.out;
        }

        class Printer final : public ConstVisitor
        {
          public:
            std::string out;

            void visit(const NodeConst &n) override { out += number(n.value); }
            void visit(const NodeVar &n) override { out += n.name; }
            void visit(const NodeSpot &n) override
            {
                out += n.index == 0 ? "spot()" : "spot(" + std::to_string(n.index) + ")";
            }

            void visit(const NodeAdd &n) override { infix(n, "+"); }
            void visit(const NodeSub &n) override { infix(n, "-"); }
            void visit(const NodeMult &n) override { infix(n, "*"); }
            void visit(const NodeDiv &n) override { infix(n, "/"); }
            void visit(const NodePow &n) override { infix(n, "^"); }
            void visit(const NodeUplus &n) override { prefix(n, "+"); }
            void visit(const NodeUminus &n) override { prefix(n, "-"); }

            void visit(const NodeMin &n) override { call(n, "min"); }
            void visit(const NodeMax &n) override { call(n, "max"); }
            void visit(const NodeLog &n) override { call(n, "log"); }
            void visit(const NodeExp &n) override { call(n, "exp"); }
            void visit(const NodeSqrt &n) override { call(n, "sqrt"); }
            void visit(const NodeAbs &n) override { call(n, "abs"); }
            void visit(const NodeSmooth &n) override { call(n, "smooth"); }

            void visit(const NodeEqual &n) override { infix(n, "="); }
            void visit(const NodeNotEqual &n) override { infix(n, "!="); }
            void visit(const NodeSuperior &n) override { infix(n, ">"); }
            void visit(const NodeSupEqual &n) override { infix(n, ">="); }
            void visit(const NodeInferior &n) override { infix(n, "<"); }
            void visit(const NodeInfEqual &n) override { infix(n, "<="); }
            void visit(const NodeAnd &n) override { infix(n, "and"); }
            void visit(const NodeOr &n) override { infix(n, "or"); }
            void visit(const NodeNot &n) override { prefix(n, "not "); }

          private:
            void infix(const Node &n, const char *op)
            {
                out += '(';
                n.arguments[0]->accept(*this);
                out += ' ';
                out += op;
                out += ' ';
                n.arguments[1]->accept(*this);
                out += ')';
            }

            void prefix(const Node &n, const char *op)
            {
                out += '(';
                out += op;
                n.arguments[0]->accept(*this);
                out += ')';
            }

            void call(const Node &n, const char *name)
            {
                out += name;
                out += '(';
                for (std::size_t k = 0; k < n.arguments.size(); ++k)
                {
                    if (k > 0)
                        out += ", ";
                    n.arguments[k]->accept(*this);
                }
                out += ')';
            }

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
        };
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_SCRIPT_WRITER_HPP
