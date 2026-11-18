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
     * own `df()` calls need, in slot order (WP 16e); `forward_mats` likewise
     * from ForwardLookupResolver, for its `fwd()` calls.
     * `barriers` comes from StepMonitorResolver: the levels the event's
     * survivalAbove() / survivalBelow() calls ask the model to monitor
     * since the previous event (WP 26 P2); empty means none.
     */
    inline std::vector<SampleDef>
    build_defline(const std::vector<Event> &events,
                  const std::vector<std::vector<Time>> &discount_mats,
                  const std::vector<std::vector<StepMonitor>> &barriers = {},
                  const std::vector<std::vector<Time>> &forward_mats = {})
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
            if (!forward_mats.empty())
                defline[i].forward_mats = forward_mats[i];
            if (!barriers.empty())
                defline[i].monitors = barriers[i];
        }
        return defline;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DEFLINE_BUILDER_HPP
