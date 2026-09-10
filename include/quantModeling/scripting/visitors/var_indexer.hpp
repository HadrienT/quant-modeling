#ifndef QM_SCRIPTING_VISITORS_VAR_INDEXER_HPP
#define QM_SCRIPTING_VISITORS_VAR_INDEXER_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Assign every distinct script variable a slot, fill NodeVar::index.
     *
     * Runs once over the whole script (all events, in order). After it,
     * `names()` gives the slot layout the evaluator's variable array uses, and
     * every NodeVar carries its slot. Variables are global to the script and
     * persist across events — a name written at one event and read at a later
     * one resolves to the same slot (blueprint/wp/16-scripting.md §4).
     */
    class VarIndexer final : public Visitor
    {
      public:
        void index(std::vector<Event> &events)
        {
            for (Event &event : events)
                for (ExprTree &statement : event.statements)
                    statement->accept(*this);
        }

        const std::vector<std::string> &names() const { return names_; }
        std::size_t count() const { return names_.size(); }

        void visit(NodeVar &n) override
        {
            const auto [it, inserted] = slot_.try_emplace(n.name, names_.size());
            if (inserted)
                names_.push_back(n.name);
            n.index = it->second;
        }

      private:
        std::vector<std::string> names_;
        std::unordered_map<std::string, std::size_t> slot_;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_VAR_INDEXER_HPP
