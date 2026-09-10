#ifndef QM_SCRIPTING_VISITORS_DEFLINE_BUILDER_HPP
#define QM_SCRIPTING_VISITORS_DEFLINE_BUILDER_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"

#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Build the vector<SampleDef> the model reads to size its output
     *        (blueprint/wp/16-scripting.md §4.3) — the repo-specific stand-in
     *        for Savine's "defline".
     *
     * v1: one SampleDef per event; `numeraire` is set on any event that pays a
     * cash flow (the amount is deflated by that date's numeraire). discount_mats
     * / forward_mats stay empty until the language gains `df()` / `fwd()`.
     */
    inline std::vector<SampleDef> build_defline(const std::vector<Event> &events)
    {
        auto pays_here = [](const Event &event)
        {
            struct Finder
            {
                bool found = false;
                void walk(const Node &node)
                {
                    if (dynamic_cast<const NodePays *>(&node))
                        found = true;
                    for (const ExprTree &child : node.arguments)
                        if (child && !found)
                            walk(*child);
                }
            } finder;
            for (const ExprTree &statement : event.statements)
                finder.walk(*statement);
            return finder.found;
        };

        std::vector<SampleDef> defline(events.size());
        for (std::size_t i = 0; i < events.size(); ++i)
            defline[i].numeraire = pays_here(events[i]);
        return defline;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DEFLINE_BUILDER_HPP
