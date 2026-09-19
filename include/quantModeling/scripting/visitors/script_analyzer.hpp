#ifndef QM_SCRIPTING_VISITORS_SCRIPT_ANALYZER_HPP
#define QM_SCRIPTING_VISITORS_SCRIPT_ANALYZER_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"

#include <cstddef>
#include <limits>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Structural facts about a script that decide which model
     *        dynamics its price actually depends on.
     *
     * A script says nothing about volatility, jumps or correlation -- the
     * model is chosen separately -- so this is the only place a mismatch
     * between the two can be detected before pricing. Every flag is a
     * *structural* fact read off the syntax tree (never a numerical
     * estimate), so it is defensible on its own.
     */
    struct ScriptAnalysis
    {
        /// spot(0..n-1) the script reads: the model needs at least this many
        /// underlyings (filled in by ScriptedProduct, which already tracks it).
        std::size_t n_underlyings = 1;

        /// A max/min/abs/smooth or comparison is applied to a spot-derived
        /// value: the price then depends on the *distribution* of spot at
        /// that date, not only its mean (a linear payoff `spot() - K` does
        /// not), so the smile at the relevant level matters.
        bool nonlinear_in_spot = false;

        /// A comparison of a spot-derived, non-flag value against a level --
        /// the shape of a barrier, a digital or an autocall trigger, the
        /// payoffs most sensitive to skew.
        bool spot_threshold_test = false;

        /// A spot-derived variable is assigned at one event and read at a
        /// later one (a running sum, a knock-in flag, a reference level such
        /// as `k = spot()`): the price depends on the *joint* law of spot
        /// across dates -- the forward smile -- not on each date's marginal
        /// alone.
        bool path_dependent = false;
    };

    namespace detail
    {
        inline bool is_spot_derived(const Node &n, const std::vector<char> &tainted)
        {
            if (dynamic_cast<const NodeSpot *>(&n))
                return true;
            if (const auto *v = dynamic_cast<const NodeVar *>(&n))
                return v->index < tainted.size() && tainted[v->index];
            for (const ExprTree &c : n.arguments)
                if (c && is_spot_derived(*c, tainted))
                    return true;
            return false;
        }

        struct Analyzer
        {
            static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

            std::vector<char> tainted;
            std::vector<std::size_t> first_assign;
            bool changed = false;
            ScriptAnalysis out;

            void grow(std::size_t idx)
            {
                if (idx >= tainted.size())
                {
                    tainted.resize(idx + 1, 0);
                    first_assign.resize(idx + 1, none);
                }
            }

            // Pass 1 (to a fixpoint): which variables carry spot-derived
            // information. A variable assigned inside an `if` whose
            // condition is spot-derived is tainted even when the assigned
            // value is a constant -- that is exactly how a knock-in flag
            // (`if spot() < 70 then ki = 1`) records the path.
            void taint(const Node &n, bool control, std::size_t event)
            {
                if (dynamic_cast<const NodeCollect *>(&n))
                {
                    taint(*n.arguments[0], control, event);
                }
                else if (dynamic_cast<const NodeAssign *>(&n))
                {
                    const auto &var = static_cast<const NodeVar &>(*n.arguments[0]);
                    grow(var.index);
                    if (first_assign[var.index] == none)
                        first_assign[var.index] = event;
                    if (!tainted[var.index] &&
                        (control || is_spot_derived(*n.arguments[1], tainted)))
                    {
                        tainted[var.index] = 1;
                        changed = true;
                    }
                }
                else if (dynamic_cast<const NodeIf *>(&n))
                {
                    const bool ct =
                        control || is_spot_derived(*n.arguments[0], tainted);
                    for (std::size_t k = 1; k < n.arguments.size(); ++k)
                        taint(*n.arguments[k], ct, event);
                }
            }

            // Pass 2: read the flags off every expression.
            void inspect(const Node &n, std::size_t event, bool is_assign_target = false)
            {
                if (const auto *v = dynamic_cast<const NodeVar *>(&n))
                {
                    if (!is_assign_target && v->index < tainted.size() &&
                        tainted[v->index] && first_assign[v->index] != none &&
                        first_assign[v->index] < event)
                        out.path_dependent = true;
                    return;
                }

                const bool nonlinear_op =
                    dynamic_cast<const NodeMax *>(&n) || dynamic_cast<const NodeMin *>(&n) ||
                    dynamic_cast<const NodeAbs *>(&n) || dynamic_cast<const NodeSmooth *>(&n);
                if (nonlinear_op && is_spot_derived(n, tainted))
                    out.nonlinear_in_spot = true;

                if (const auto *cmp = dynamic_cast<const NodeComparison *>(&n))
                {
                    if (!cmp->alwaysTrue && !cmp->alwaysFalse &&
                        is_spot_derived(n, tainted))
                    {
                        out.nonlinear_in_spot = true;
                        if (!cmp->discrete)
                            out.spot_threshold_test = true;
                    }
                }

                const bool assign = dynamic_cast<const NodeAssign *>(&n) != nullptr;
                for (std::size_t k = 0; k < n.arguments.size(); ++k)
                    if (n.arguments[k])
                        inspect(*n.arguments[k], event, assign && k == 0);
            }
        };
    } // namespace detail

    /// Events must already be in date order (ScriptedProduct sorts them) and
    /// run through VarIndexer, ConstCondProcessor and DomainProcessor, so
    /// variable slots, always-true/false conditions and `discrete` are set.
    inline ScriptAnalysis analyze_script(const std::vector<Event> &events)
    {
        detail::Analyzer a;
        do
        {
            a.changed = false;
            for (std::size_t e = 0; e < events.size(); ++e)
                for (const ExprTree &s : events[e].statements)
                    if (s)
                        a.taint(*s, false, e);
        } while (a.changed);

        for (std::size_t e = 0; e < events.size(); ++e)
            for (const ExprTree &s : events[e].statements)
                if (s)
                    a.inspect(*s, e);
        return a.out;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_SCRIPT_ANALYZER_HPP
