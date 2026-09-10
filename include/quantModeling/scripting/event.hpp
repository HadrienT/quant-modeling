#ifndef QM_SCRIPTING_EVENT_HPP
#define QM_SCRIPTING_EVENT_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/scripting/node.hpp"

#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief One dated block of a script: `(date, statements)`.
     *
     * The parser emits `std::vector<Event>` (WP 16-scripting §3). A shared date
     * line applies the same block to several dates — it produces one Event per
     * date, each holding an independent deep copy of the statements. Each entry
     * of `statements` is a NodeCollect wrapping a single NodeAssign / NodePays /
     * NodeIf.
     *
     * Dates are calendar dates (ADR-S5). Resolution to a year-fraction Time
     * happens later, in ScriptedProduct's constructor via ValuationContext —
     * out of scope for lot 16a.
     */
    struct Event
    {
        Date date;
        std::vector<ExprTree> statements;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_EVENT_HPP
