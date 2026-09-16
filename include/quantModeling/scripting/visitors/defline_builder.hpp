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
     * One SampleDef per event; `numeraire` is set on any event that pays a
     * cash flow (the amount is deflated by that date's numeraire).
     * `discount_mats` comes from `discount_lookup_resolver.hpp`'s pass over
     * the same, already-resolved event list — the maturities that event's
     * own `df()` calls need, in slot order (WP 16e). `forward_mats` stays
     * empty until the language gains `fwd()` (still out of scope: it needs
     * a real forward curve, not the flat-r convention `df()` reuses).
     */
    inline std::vector<SampleDef>
    build_defline(const std::vector<Event> &events,
                  const std::vector<std::vector<Time>> &discount_mats)
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
        {
            defline[i].numeraire = pays_here(events[i]);
            defline[i].discount_mats = discount_mats[i];
        }
        return defline;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DEFLINE_BUILDER_HPP
