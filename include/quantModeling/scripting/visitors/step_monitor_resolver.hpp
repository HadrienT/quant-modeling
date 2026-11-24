#ifndef QM_SCRIPTING_VISITORS_STEP_MONITOR_RESOLVER_HPP
#define QM_SCRIPTING_VISITORS_STEP_MONITOR_RESOLVER_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <algorithm>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Give every `survivalAbove(L)` / `survivalBelow(L)` (WP 26 P2)
     *        and every `lastStepCall(K)` / `lastStepPut(K)` /
     *        `lastStepDigital(K)` (WP 27 §8 rung 1) a slot in its own
     *        event's list of model-computed quantities.
     *
     * The same route as df() (DiscountLookupResolver): a slot is local to
     * the event, it indexes that event's SampleDef::monitors and
     * Sample::monitored, and the model fills the survivals while it builds
     * the path. Two calls with the same level, side and asset within one
     * event share a slot.
     */
    class StepMonitorResolver final : public Visitor
    {
      public:
        /// One inner vector per event, same order as `events`.
        std::vector<std::vector<StepMonitor>> resolve(std::vector<Event> &events)
        {
            std::vector<std::vector<StepMonitor>> out(events.size());
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

        void visit(NodeStepQuantity &n) override
        {
            if (n.computed_strike())
            {
                // The strike is the script's: the model hands over the last
                // step's forward and log-variance (shared by every such call
                // on the asset), the payoff does the rest.
                visit_children(n);
                n.slot = slot_of(StepMonitor{0.0, n.asset, true, StepQuantity::LastStepForward});
                n.var_slot = slot_of(StepMonitor{0.0, n.asset, true, StepQuantity::LastStepVariance});
                return;
            }
            // A last-step expectation has no side: `above` is left at its
            // default so that two calls with the same strike share a slot.
            const bool survival = n.kind == StepQuantity::Survival;
            const StepMonitor want{n.level, n.asset, survival ? n.above : true, n.kind};
            const auto it = std::find(current_->begin(), current_->end(), want);
            n.slot = static_cast<std::size_t>(it - current_->begin());
            if (it == current_->end())
                current_->push_back(want);
        }

      private:
        std::size_t slot_of(const StepMonitor &want)
        {
            const auto it = std::find(current_->begin(), current_->end(), want);
            const auto slot = static_cast<std::size_t>(it - current_->begin());
            if (it == current_->end())
                current_->push_back(want);
            return slot;
        }

        std::vector<StepMonitor> *current_ = nullptr;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_STEP_MONITOR_RESOLVER_HPP
