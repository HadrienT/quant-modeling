#ifndef QM_SCRIPTING_VISITORS_DISCOUNT_LOOKUP_RESOLVER_HPP
#define QM_SCRIPTING_VISITORS_DISCOUNT_LOOKUP_RESOLVER_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Resolve every `df(DATE)` to a Time and a slot in its own event's
     *        discount lookups (WP 16e).
     *
     * Runs once, after timeline resolution (so it walks the final,
     * post-merge event list -- one entry per element of the product's own
     * timeline/defline) and before DeflineBuilder. Unlike VarIndexer's
     * slots, which are global to the whole script, a df() slot is local to
     * the event it appears in: it indexes that event's own
     * SampleDef::discount_mats / Sample::discounts, mirroring how a model
     * populates those fields from a flat r today (models/equity/bs_sim_
     * model.hpp and friends) -- df() needs no new market-data plumbing, just
     * a route from the AST to fields that already exist.
     *
     * Two df() calls for the same maturity within one event share a slot:
     * no reason to carry the same discount factor twice in a Sample.
     */
    class DiscountLookupResolver final : public Visitor
    {
      public:
        explicit DiscountLookupResolver(const ValuationContext &ctx) : ctx_(ctx) {}

        /// One inner vector per event, same order as `events` -- the
        /// maturities that event's df() calls need, in slot order.
        std::vector<std::vector<Time>> resolve(std::vector<Event> &events)
        {
            std::vector<std::vector<Time>> mats(events.size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                current_ = &mats[i];
                for (ExprTree &statement : events[i].statements)
                    statement->accept(*this);
            }
            current_ = nullptr;
            return mats;
        }

        void visit(NodeDf &n) override
        {
            n.maturity = ctx_.t(n.date);
            const auto it = std::find(current_->begin(), current_->end(), n.maturity);
            if (it == current_->end())
            {
                n.slot = current_->size();
                current_->push_back(n.maturity);
            }
            else
            {
                n.slot = static_cast<std::size_t>(it - current_->begin());
            }
        }

      private:
        const ValuationContext &ctx_;
        std::vector<Time> *current_ = nullptr;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_DISCOUNT_LOOKUP_RESOLVER_HPP
