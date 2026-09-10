#ifndef QM_SCRIPTING_VISITORS_IF_PROCESSOR_HPP
#define QM_SCRIPTING_VISITORS_IF_PROCESSOR_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <set>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Fill NodeIf::affectedVars — the variable slots written by either
     *        branch (blueprint/wp/16-scripting.md §4.1).
     *
     * Needed twice later: in hard evaluation the `else` branch restores the
     * variables the `then` branch did not write; in fuzzy evaluation (WP 16c)
     * it is the exact set of variables to blend between the two branches.
     * Requires VarIndexer to have run (indices populated).
     */
    class IfProcessor final : public Visitor
    {
      public:
        void process(std::vector<Event> &events)
        {
            for (Event &event : events)
                for (ExprTree &statement : event.statements)
                    statement->accept(*this);
        }

        void visit(NodeIf &n) override
        {
            visit_children(n); // nested ifs first

            std::set<std::size_t> written;
            for (std::size_t k = 1; k < n.arguments.size(); ++k)
                collect_assigned(*n.arguments[k], written);
            n.affectedVars.assign(written.begin(), written.end());
        }

      private:
        static void collect_assigned(const Node &node, std::set<std::size_t> &out)
        {
            if (const auto *assign = dynamic_cast<const NodeAssign *>(&node))
            {
                const auto &var =
                    static_cast<const NodeVar &>(*assign->arguments[0]);
                out.insert(var.index);
                return;
            }
            for (const ExprTree &child : node.arguments)
                if (child)
                    collect_assigned(*child, out);
        }
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_IF_PROCESSOR_HPP
