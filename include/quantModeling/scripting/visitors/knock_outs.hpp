#ifndef QM_SCRIPTING_VISITORS_KNOCK_OUTS_HPP
#define QM_SCRIPTING_VISITORS_KNOCK_OUTS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/visitor.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * @file knock_outs.hpp
 * @brief knockOutAbove(L) / knockOutBelow(L): a knock-out the model handles by
 *        conditioning the step into its date on survival (issue #369).
 *
 * Glasserman & Staum (2001), "Conditioning on one-step survival for barrier
 * option simulations": at each knock-out date the model draws the step into
 * the date from the normal truncated to the safe side and hands over p, the
 * probability of that side given the path before the step. The path then
 * never dies; it carries the weight prod p, and every later payment is
 * multiplied by it. Unbiased, and the payoff has no indicator left: smooth in
 * the parameters along a path (pathwise greeks without fuzzy smoothing), and
 * the multilevel levels no longer flip across the barrier.
 *
 * The parser writes `knockOutAbove(L)` as `#knockout = #knockout * w`, w a
 * NodeStepQuantity of kind KnockOut. Two passes here finish the job:
 *
 *  - prepare_knock_outs(), on the parsed events in date order: refuses a
 *    knock-out inside an `if` and the scripts it cannot weight (controlPays,
 *    exercise), moves each date's knock-outs to the front of its statements
 *    -- the weight of a date applies to that date's payments too, whatever
 *    the order written -- starts the weight at 1 on the first date, and
 *    multiplies every `pays` from the first knock-out's date on by it;
 *  - resolve_knock_out_levels(), once the historical fixings are replayed:
 *    the model needs the level before it simulates, so it must be known
 *    today -- numbers, variables set on past dates (`s0 = spot()` on a fixing
 *    already taken), or set once on a future date to such an expression (a
 *    term, `barrier = 1.3`). It is evaluated and moved to the node's `level`.
 */

namespace quantModeling::scripting
{

    namespace knock_out_detail
    {
        inline bool is_weight(const Node &n)
        {
            const auto *s = dynamic_cast<const NodeStepQuantity *>(&n);
            return s != nullptr && s->kind == StepQuantity::KnockOut;
        }

        /// The statement the parser writes for knockOutAbove / Below.
        inline NodeStepQuantity *statement_weight(const ExprTree &statement)
        {
            if (!statement || statement->arguments.empty())
                return nullptr;
            auto *assign = dynamic_cast<NodeAssign *>(statement->arguments[0].get());
            if (assign == nullptr || assign->arguments.size() != 2)
                return nullptr;
            const auto *target = dynamic_cast<const NodeVar *>(assign->arguments[0].get());
            auto *product = dynamic_cast<NodeMult *>(assign->arguments[1].get());
            if (target == nullptr || target->name != NodeStepQuantity::kKnockOutVariable || product == nullptr ||
                product->arguments.size() != 2)
                return nullptr;
            auto *weight = dynamic_cast<NodeStepQuantity *>(product->arguments[1].get());
            return weight != nullptr && weight->kind == StepQuantity::KnockOut ? weight : nullptr;
        }

        /// Counts the knock-out weights, controlPays and exercise() below a node.
        class Census final : public ConstVisitor
        {
          public:
            std::size_t weights = 0;
            bool control = false;
            bool exercise = false;

            void visit(const NodeStepQuantity &n) override
            {
                if (n.kind == StepQuantity::KnockOut)
                    ++weights;
                visit_children(n);
            }
            void visit(const NodeControlPays &n) override
            {
                control = true;
                visit_children(n);
            }
            void visit(const NodeExercise &n) override
            {
                exercise = true;
                visit_children(n);
            }
        };

        inline ExprTree weight_variable()
        {
            auto v = std::make_unique<NodeVar>();
            v->name = NodeStepQuantity::kKnockOutVariable;
            return v;
        }

        /// pays X -> pays #knockout * X, inside ifs too.
        class WeighPayments final : public Visitor
        {
          public:
            void visit(NodePays &n) override
            {
                auto product = std::make_unique<NodeMult>();
                product->arguments.push_back(weight_variable());
                product->arguments.push_back(std::move(n.arguments[0]));
                n.arguments[0] = std::move(product);
            }
        };
    } // namespace knock_out_detail

    /// See the file comment. Returns whether the script has a knock-out.
    inline bool prepare_knock_outs(std::vector<Event> &events)
    {
        using namespace knock_out_detail;
        std::size_t first = events.size(), at_top = 0;
        Census census;
        for (std::size_t i = 0; i < events.size(); ++i)
            for (const ExprTree &statement : events[i].statements)
            {
                if (statement)
                    statement->accept(census);
                if (statement_weight(statement) != nullptr)
                {
                    ++at_top;
                    first = std::min(first, i);
                }
            }
        if (census.weights == 0)
            return false;
        if (census.weights != at_top)
            throw InvalidInput("knockOutAbove() / knockOutBelow() is a statement of its own on a date, not part of "
                               "an expression or inside an if");
        if (census.control)
            throw InvalidInput("knockOutAbove() / knockOutBelow() cannot be combined with controlPays: the "
                               "control's known mean is that of the unconditioned paths");
        if (census.exercise)
            throw InvalidInput("knockOutAbove() / knockOutBelow() cannot be combined with exercise() / call() yet");

        for (Event &event : events)
            std::stable_partition(event.statements.begin(), event.statements.end(),
                                  [](const ExprTree &s)
                                  { return statement_weight(s) != nullptr; });

        auto one = std::make_unique<NodeConst>();
        one->value = 1.0;
        auto init = std::make_unique<NodeAssign>();
        init->arguments.push_back(weight_variable());
        init->arguments.push_back(std::move(one));
        auto collect = std::make_unique<NodeCollect>();
        collect->arguments.push_back(std::move(init));
        events.front().statements.insert(events.front().statements.begin(), std::move(collect));

        WeighPayments weigh;
        for (std::size_t i = first; i < events.size(); ++i)
            for (ExprTree &statement : events[i].statements)
                if (statement)
                    statement->accept(weigh);
        return true;
    }

    namespace knock_out_detail
    {
        /// Folds a knock-out level to a number (see resolve_knock_out_levels).
        class LevelFolder final : public ConstVisitor
        {
          public:
            LevelFolder(const std::vector<double> &baseline,
                        const std::map<std::size_t, std::pair<const Node *, std::size_t>> &assigned_once,
                        const std::vector<char> &assigned_more)
                : baseline_(baseline), once_(assigned_once), more_(assigned_more)
            {
            }

            /// The event of the knock-out being resolved.
            std::size_t event = 0;

            double fold(const Node &n)
            {
                n.accept(*this);
                return value_;
            }

            void visit(const NodeConst &n) override { value_ = n.value; }
            void visit(const NodeVar &n) override
            {
                if (n.index < more_.size() && more_[n.index])
                    unknown("'" + n.name + "' is set more than once, or inside an if");
                const auto it = once_.find(n.index);
                if (it != once_.end())
                {
                    if (it->second.second > event)
                        unknown("'" + n.name + "' is set after this date");
                    if (++depth_ > 64)
                        unknown("'" + n.name + "' is defined in terms of itself");
                    value_ = fold(*it->second.first);
                    --depth_;
                    return;
                }
                value_ = n.index < baseline_.size() ? baseline_[n.index] : 0.0;
            }
            void visit(const NodeAdd &n) override
            {
                binary(n, [](double a, double b)
                       { return a + b; });
            }
            void visit(const NodeSub &n) override
            {
                binary(n, [](double a, double b)
                       { return a - b; });
            }
            void visit(const NodeMult &n) override
            {
                binary(n, [](double a, double b)
                       { return a * b; });
            }
            void visit(const NodeDiv &n) override
            {
                binary(n, [](double a, double b)
                       { return a / b; });
            }
            void visit(const NodePow &n) override
            {
                binary(n, [](double a, double b)
                       { return std::pow(a, b); });
            }
            void visit(const NodeMin &n) override
            {
                binary(n, [](double a, double b)
                       { return std::min(a, b); });
            }
            void visit(const NodeMax &n) override
            {
                binary(n, [](double a, double b)
                       { return std::max(a, b); });
            }
            void visit(const NodeUplus &n) override { value_ = fold(*n.arguments[0]); }
            void visit(const NodeUminus &n) override { value_ = -fold(*n.arguments[0]); }
            void visit(const NodeLog &n) override { value_ = std::log(fold(*n.arguments[0])); }
            void visit(const NodeExp &n) override { value_ = std::exp(fold(*n.arguments[0])); }
            void visit(const NodeSqrt &n) override { value_ = std::sqrt(fold(*n.arguments[0])); }
            void visit(const NodeAbs &n) override { value_ = std::abs(fold(*n.arguments[0])); }
            void visit(const NodeSpot &) override { unknown("it reads a spot still to be simulated"); }
            void visit(const NodeDf &) override { unknown("it reads df()"); }
            void visit(const NodeFwd &) override { unknown("it reads fwd()"); }
            void visit(const NodeStepQuantity &) override { unknown("it reads a model quantity"); }

          private:
            template <class F>
            void binary(const Node &n, F f)
            {
                const double a = fold(*n.arguments[0]);
                const double b = fold(*n.arguments[1]);
                value_ = f(a, b);
            }
            [[noreturn]] static void unknown(const std::string &why)
            {
                throw InvalidInput("the level of knockOutAbove() / knockOutBelow() must be known today (numbers, "
                                   "terms, fixings already taken): " +
                                   why);
            }

            const std::vector<double> &baseline_;
            const std::map<std::size_t, std::pair<const Node *, std::size_t>> &once_;
            const std::vector<char> &more_;
            double value_ = 0.0;
            int depth_ = 0;
        };

        /// The variables the future events assign: once at the top of an
        /// event (its right-hand side kept), or more than that / inside an if.
        class Assignments final : public ConstVisitor
        {
          public:
            explicit Assignments(std::size_t n_vars)
                : more(n_vars, 0) {}
            std::map<std::size_t, std::pair<const Node *, std::size_t>> once;
            std::vector<char> more;
            bool top = true;
            std::size_t event = 0;

            void visit(const NodeIf &n) override
            {
                const bool was = top;
                top = false;
                visit_children(n);
                top = was;
            }
            void visit(const NodeAssign &n) override
            {
                const auto *v = dynamic_cast<const NodeVar *>(n.arguments[0].get());
                if (v != nullptr && v->index < more.size() && v->name != NodeStepQuantity::kKnockOutVariable)
                {
                    if (!top || once.count(v->index) != 0)
                    {
                        more[v->index] = 1;
                        once.erase(v->index);
                    }
                    else if (!more[v->index])
                        once[v->index] = {n.arguments[1].get(), event};
                }
                visit_children(n);
            }
        };

        class Resolver final : public Visitor
        {
          public:
            explicit Resolver(LevelFolder &folder)
                : folder_(folder) {}
            void visit(NodeStepQuantity &n) override
            {
                if (n.kind != StepQuantity::KnockOut || n.arguments.empty())
                    return visit_children(n);
                const double level = folder_.fold(*n.arguments[0]);
                if (!(level > 0.0) || !std::isfinite(level))
                    throw InvalidInput(std::string(n.function()) + "(L): L must be a positive number, got " +
                                       std::to_string(level));
                n.level = level;
                n.arguments.clear();
            }

          private:
            LevelFolder &folder_;
        };
    } // namespace knock_out_detail

    /// See the file comment. `future`: the events still to simulate, their
    /// variables indexed; `baseline`: the variables after the historical
    /// fixings (empty without any).
    inline void resolve_knock_out_levels(std::vector<Event> &future, const std::vector<double> &baseline,
                                         std::size_t n_vars)
    {
        using namespace knock_out_detail;
        Assignments assignments(n_vars);
        for (std::size_t i = 0; i < future.size(); ++i)
        {
            assignments.event = i;
            for (const ExprTree &statement : future[i].statements)
                if (statement)
                    statement->accept(assignments);
        }
        LevelFolder folder(baseline, assignments.once, assignments.more);
        Resolver resolver(folder);
        for (std::size_t i = 0; i < future.size(); ++i)
        {
            folder.event = i;
            for (ExprTree &statement : future[i].statements)
                if (statement)
                    statement->accept(resolver);
        }
    }

    /// Whether any of these events holds a knock-out (a past date may not).
    inline bool has_knock_out(const std::vector<Event> &events)
    {
        knock_out_detail::Census census;
        for (const Event &event : events)
            for (const ExprTree &statement : event.statements)
                if (statement)
                    statement->accept(census);
        return census.weights > 0;
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_VISITORS_KNOCK_OUTS_HPP
