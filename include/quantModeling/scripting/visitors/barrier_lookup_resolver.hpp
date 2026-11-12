#ifndef QM_SCRIPTING_VISITORS_BARRIER_LOOKUP_RESOLVER_HPP
#define QM_SCRIPTING_VISITORS_BARRIER_LOOKUP_RESOLVER_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <algorithm>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Give every `survivalAbove(L)` / `survivalBelow(L)` a slot in its
     *        own event's barrier list (WP 26 P2).
     *
     * The same route as df() (DiscountLookupResolver): a slot is local to
     * the event, it indexes that event's SampleDef::barriers and
     * Sample::survivals, and the model fills the survivals while it builds
     * the path. Two calls with the same level, side and asset within one
     * event share a slot.
     */
    class BarrierLookupResolver final : public Visitor
    {
      public:
        /// One inner vector per event, same order as `events`.
        std::vector<std::vector<BarrierMonitor>> resolve(std::vector<Event> &events)
        {
            std::vector<std::vector<BarrierMonitor>> out(events.size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                current_ = &out[i];
                for (ExprTree &statement : events[i].statements)
                    if (statement)
                        statement->accept(*this);
            }
            current_ = nullptr;
            return out;
        }

        void visit(NodeSurvival &n) override
        {
            const BarrierMonitor want{n.level, n.asset, n.above};
            const auto it = std::find(current_->begin(), current_->end(), want);
            n.slot = static_cast<std::size_t>(it - current_->begin());
            if (it == current_->end())
                current_->push_back(want);
        }

      private:
        std::vector<BarrierMonitor> *current_ = nullptr;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_BARRIER_LOOKUP_RESOLVER_HPP
