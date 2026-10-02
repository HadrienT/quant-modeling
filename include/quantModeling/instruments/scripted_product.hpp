#ifndef QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP
#define QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/bytecode.hpp"
#include "quantModeling/scripting/compiler.hpp"
#include "quantModeling/scripting/evaluator.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/exercise.hpp"
#include "quantModeling/scripting/fuzzy_evaluator.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/const_cond.hpp"
#include "quantModeling/scripting/visitors/defline_builder.hpp"
#include "quantModeling/scripting/visitors/discount_lookup_resolver.hpp"
#include "quantModeling/scripting/visitors/domain_processor.hpp"
#include "quantModeling/scripting/visitors/if_processor.hpp"
#include "quantModeling/scripting/visitors/script_analyzer.hpp"
#include "quantModeling/scripting/visitors/var_indexer.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quantModeling
{

    namespace detail
    {
        /// Highest asset index any spot(i) in the subtree asks for (0 for a
        /// script that only ever uses spot()). A plain recursive walk over
        /// every node's arguments, not a full ConstVisitor pass: NodeSpot is
        /// a leaf and every node type already stores its children in
        /// `arguments`, so nothing type-specific is needed the way
        /// try_eval_const's semantic evaluation does.
        inline std::size_t max_spot_index(const scripting::Node &node)
        {
            std::size_t m = 0;
            if (const auto *s = dynamic_cast<const scripting::NodeSpot *>(&node))
                m = s->index;
            for (const scripting::ExprTree &child : node.arguments)
                if (child)
                    m = std::max(m, max_spot_index(*child));
            return m;
        }

        /// The exercise() / call() nodes of a subtree, in evaluation order.
        inline void collect_exercises(const scripting::Node &node,
                                      std::vector<const scripting::NodeExercise *> &out)
        {
            if (const auto *e = dynamic_cast<const scripting::NodeExercise *>(&node))
                out.push_back(e);
            for (const scripting::ExprTree &child : node.arguments)
                if (child)
                    collect_exercises(*child, out);
        }
    } // namespace detail

    /// One pilot path of an early-exercise script, in deflated currency: its
    /// value if never exercised, and for each exercise date it reached, the
    /// regressors there and its value if exercised there (LSMC, lsm.hpp).
    struct ExercisePilotPath
    {
        double never = 0.0;
        struct Opportunity
        {
            std::size_t event;
            std::vector<double> regressors;
            double exercised;
        };
        std::vector<Opportunity> opportunities;
    };

    struct ScriptSettings
    {
        bool fuzzy = false; ///< 16c — FuzzyEvaluator; hard only for now
        double default_eps = 0.01;
        /// Price through the compiled bytecode (blueprint/wp/19-gpu.md §3,
        /// lot G2) rather than by walking the tree. Same numbers to the bit
        /// (tests/testScriptBytecode.cpp); the tree evaluators remain the
        /// oracle, and the bytecode is what the GPU runs.
        bool bytecode = true;
    };

    /**
     * @brief A product described by a script, priced by the one generic
     *        Monte-Carlo engine (blueprint/wp/16-scripting.md §6).
     *
     * ScriptedProduct<T> is just one more ISimulatableProduct<T>: no engine, no
     * model and no sampler is modified. The constructor runs the whole
     * front-end once — parse, VarIndexer, ConstCondProcessor, IfProcessor,
     * DomainProcessor, DeflineBuilder — then resolves the script's calendar
     * dates to the numeric timeline through the ValuationContext (ADR-S5).
     * payoffs() replays the events of one simulated path through an
     * Evaluator<T>: hard by default, or the FuzzyEvaluator<T> when
     * ScriptSettings::fuzzy — a payoff smoothed for a correct pathwise delta on
     * digitals and barriers.
     *
     * `spot()` reads asset 0; `spot(i)` reads asset i, for a script that
     * needs more than one underlying (blueprint/wp/17-aad.md lot 17e) --
     * n_underlyings() reports the highest index used, plus one, so a caller
     * knows how many assets the model it builds needs to carry.
     *
     * An event on or before the valuation date is a **historical fixing**
     * (lot 16e): it is not simulated (there is no future path to a past
     * date), so it is replayed exactly once at construction, against
     * caller-supplied fixing values, through a plain hard Evaluator<T> --
     * a fixing already happened, so there is nothing left to smooth even
     * when `settings.fuzzy` is set for the live pricing. Only the resulting
     * *variable state* carries forward, as the starting point every future
     * path's Evaluator::initialize() resets to instead of all-zero; any
     * `pays` inside a historical event is a sunk cash flow and is dropped,
     * not counted in today's price (Evaluator::set_suppress_payoff()).
     * `df()` is not supported in a historical event: it has no model yet to
     * ask for a discount factor at construction time.
     */
    template <class T = Real>
    class ScriptedProduct final : public ISimulatableProduct<T>
    {
      public:
        ScriptedProduct(const std::string &script, const ValuationContext &ctx,
                        const ScriptSettings &settings = {},
                        const std::map<Date, std::vector<double>>
                            &historical_fixings = {})
        {
            std::vector<scripting::Event> events = scripting::parse_script(script);
            std::stable_sort(events.begin(), events.end(),
                             [](const scripting::Event &a,
                                const scripting::Event &b)
                             { return a.date < b.date; });

            scripting::VarIndexer indexer;
            indexer.index(events);
            variable_names_ = indexer.names();

            scripting::ConstCondProcessor().process(events);
            scripting::IfProcessor().process(events);
            scripting::DomainProcessor(indexer.count(), settings.default_eps)
                .process(events);

            analysis_ = scripting::analyze_script(events);

            resolve_timeline(std::move(events), ctx);

            for (const scripting::Event &event : events_)
                for (const scripting::ExprTree &statement : event.statements)
                    if (statement)
                        n_underlyings_ = std::max(
                            n_underlyings_, detail::max_spot_index(*statement) + 1);

            analysis_.n_underlyings = n_underlyings_;

            std::vector<const scripting::NodeExercise *> exercises;
            for (const scripting::Event &event : events_)
                for (const scripting::ExprTree &statement : event.statements)
                    if (statement)
                        detail::collect_exercises(*statement, exercises);
            for (const scripting::Event &event : historical_events_)
                for (const scripting::ExprTree &statement : event.statements)
                    if (statement)
                    {
                        std::vector<const scripting::NodeExercise *> past;
                        detail::collect_exercises(*statement, past);
                        if (!past.empty())
                            throw InvalidInput("exercise() / call() on a past date (" + event.date.to_iso() +
                                               "): the decision was already taken; script its outcome instead");
                    }
            has_exercise_ = !exercises.empty();
            for (const scripting::NodeExercise *e : exercises)
            {
                if (e->issuer != exercises.front()->issuer)
                    throw InvalidInput("a script has either exercise() (the holder's right) or call() (the "
                                       "issuer's), not both");
                if (e->arguments.size() != exercises.front()->arguments.size())
                    throw InvalidInput("every exercise() / call() of a script takes the same number of regressors");
            }
            policy_.issuer = has_exercise_ && exercises.front()->issuer;

            const std::vector<std::vector<Time>> discount_mats =
                scripting::DiscountLookupResolver(ctx).resolve(events_);
            defline_ = scripting::build_defline(events_, discount_mats);

            const std::vector<std::vector<Time>> historical_mats =
                scripting::DiscountLookupResolver(ctx).resolve(historical_events_);
            for (std::size_t i = 0; i < historical_mats.size(); ++i)
                if (!historical_mats[i].empty())
                    throw InvalidInput(
                        "ScriptedProduct: df() is not supported in a "
                        "historical (pre-valuation) event (" +
                        historical_events_[i].date.to_iso() + ")");

            if (settings.fuzzy)
                evaluator_ = std::make_unique<scripting::FuzzyEvaluator<T>>();
            else
                evaluator_ = std::make_unique<scripting::Evaluator<T>>();
            evaluator_->set_variable_count(indexer.count());

            if (!historical_events_.empty())
            {
                baseline_ = replay_historical(indexer.count(), historical_fixings);
                evaluator_->set_baseline(baseline_);
            }

            // Early exercise runs on the tree evaluator only: the bytecode (and
            // with it the GPU) has no exercise instruction.
            if (settings.bytecode && !has_exercise_)
            {
                program_ = scripting::compile_script(events_, indexer.count(), settings.fuzzy);
                vars_.assign(static_cast<std::size_t>(program_.n_vars), T(0));
                stack_.assign(static_cast<std::size_t>(program_.max_stack), T(0));
                degrees_.assign(static_cast<std::size_t>(program_.max_degrees), T(0));
                if_slots_.assign(static_cast<std::size_t>(program_.n_if_slots), T(0));
                if_mode_.assign(static_cast<std::size_t>(program_.n_if_modes), 0);
                compiled_ = true;
            }
        }

        /// The script has exercise() or call(): it prices only once an
        /// exercise rule is set (fit_exercise_policy, engines/mc/lsm.hpp).
        bool has_exercise() const { return has_exercise_; }
        bool issuer_exercise() const { return policy_.issuer; }

        /// The exercise rule payoffs() applies from now on.
        void set_exercise_policy(scripting::ExercisePolicy policy)
        {
            policy_ = std::move(policy);
            evaluator_->set_exercise_policy(&policy_);
        }
        const scripting::ExercisePolicy &exercise_policy() const { return policy_; }

        /**
         * @brief The pilot's view of one path: the script run once without
         *        exercising, then once per exercise date it reached with the
         *        right exercised there. Leaves the product under its rule.
         */
        ExercisePilotPath exercise_pilot(const Scenario<T> &path) const
        {
            using Eval = scripting::Evaluator<T>;
            ExercisePilotPath out;
            std::vector<typename Eval::ExerciseObservation> seen;
            // The never-exercised run, keeping the state at the start of
            // every event: a branch exercised at event e is the same path up
            // to e, so it restarts there instead of from the first event.
            std::vector<typename Eval::State> at_start;
            at_start.reserve(events_.size());
            evaluator_->set_exercise_never();
            evaluator_->record_exercises(&seen);
            evaluator_->initialize();
            for (std::size_t i = 0; i < events_.size(); ++i)
            {
                at_start.push_back(evaluator_->state());
                run_event(path, i);
            }
            out.never = static_cast<double>(evaluator_->payoff());
            evaluator_->record_exercises(nullptr);
            for (auto &obs : seen)
            {
                evaluator_->set_exercise_at(obs.event);
                evaluator_->restore(at_start[obs.event]);
                for (std::size_t i = obs.event; i < events_.size(); ++i)
                    run_event(path, i);
                out.opportunities.push_back(
                    {obs.event, std::move(obs.regressors), static_cast<double>(evaluator_->payoff())});
            }
            evaluator_->set_exercise_policy(policy_.empty() ? nullptr : &policy_);
            return out;
        }

        /// The compiled script (empty when ScriptSettings::bytecode is off).
        const scripting::Program &program() const { return program_; }
        /// Variable state every future path starts from (empty: all zero).
        const std::vector<T> &baseline() const { return baseline_; }

        const TimeLine &timeline() const override { return timeline_; }
        const std::vector<SampleDef> &defline() const override { return defline_; }
        const std::vector<std::string> &payoff_labels() const override
        {
            return labels_;
        }
        /// Highest spot(i) index used in the script, plus one -- 1 for a
        /// script that only ever calls spot(). A caller pricing this product
        /// must supply a model with at least this many underlyings (see
        /// models/equity/multi_asset_bs_sim_model.hpp); the evaluator itself
        /// bounds-checks every spot(i) access rather than trust that they do
        /// (scripting/evaluator.hpp, scripting/fuzzy_evaluator.hpp).
        std::size_t n_underlyings() const override { return n_underlyings_; }

        /// Which model dynamics this script's price depends on (see
        /// scripting/visitors/script_analyzer.hpp) -- input to
        /// scripting/model_advice.hpp's warnings.
        const scripting::ScriptAnalysis &analysis() const { return analysis_; }

        const std::vector<std::string> &variable_names() const
        {
            return variable_names_;
        }

        /// The calendar date of each timeline() entry, same order — for
        /// diagnostics (e.g. a "validate this script" UI): what the parser
        /// actually resolved each event to.
        std::vector<Date> event_dates() const
        {
            std::vector<Date> out;
            out.reserve(events_.size());
            for (const scripting::Event &event : events_)
                out.push_back(event.date);
            return out;
        }

        void payoffs(const Scenario<T> &path, std::vector<T> &out) const override
        {
            if (compiled_)
                return payoffs_compiled(path, out);
            out.assign(1, run_tree(path));
        }

        /**
         * @brief One path, event by event: after each event, the payoff
         *        accumulated so far — each `pays` divided by the scenario's
         *        numeraire at its event — and, on request, the variables.
         *
         * With a numeraire of 1 at every event the increments of
         * `cumulative` are the cash flows themselves, date by date: what an
         * exposure engine needs (engines/xva/script_future_value.hpp), where
         * payoffs() only gives their deflated sum. Runs the tree evaluator,
         * under the exercise rule that is set.
         *
         * @param cumulative one value per timeline() entry.
         * @param variables  when not null: timeline().size() rows of
         *                   variable_names().size() values, row by row — the
         *                   state after each event.
         */
        void replay(const Scenario<T> &path, std::vector<T> &cumulative,
                    std::vector<T> *variables = nullptr) const
        {
            cumulative.resize(events_.size());
            if (variables != nullptr)
                variables->clear();
            evaluator_->initialize();
            for (std::size_t i = 0; i < events_.size(); ++i)
            {
                run_event(path, i);
                cumulative[i] = evaluator_->payoff();
                if (variables != nullptr)
                    variables->insert(variables->end(), evaluator_->variables().begin(),
                                      evaluator_->variables().end());
            }
        }

      private:
        T run_tree(const Scenario<T> &path) const
        {
            evaluator_->initialize();
            for (std::size_t i = 0; i < events_.size(); ++i)
                run_event(path, i);
            return evaluator_->payoff();
        }

        void run_event(const Scenario<T> &path, std::size_t i) const
        {
            evaluator_->set_event(path, i);
            for (const scripting::ExprTree &statement : events_[i].statements)
                evaluator_->run(*statement);
        }

        /// The bytecode path: the tree evaluator's semantics, including its
        /// run-time checks on what the model supplies.
        void payoffs_compiled(const Scenario<T> &path, std::vector<T> &out) const
        {
            if (baseline_.empty())
                std::fill(vars_.begin(), vars_.end(), T(0));
            else
                vars_ = baseline_;
            scripting::Machine<T> m{vars_.data(), stack_.data(), degrees_.data(), if_slots_.data(),
                                    if_mode_.data(), T(0)};
            const scripting::ProgramView view = program_.view();
            for (std::size_t i = 0; i < events_.size(); ++i)
            {
                const Sample<T> &smp = path[i];
                const std::int32_t max_spot = program_.event_max_spot[i];
                if (max_spot >= 0 && static_cast<std::size_t>(max_spot) >= smp.spots.size())
                    throw InvalidInput("spot(" + std::to_string(max_spot) + "): the model only carries " +
                                       std::to_string(smp.spots.size()) +
                                       " underlying(s) -- ScriptedProduct::n_underlyings() reports "
                                       "what the script actually needs");
                const std::int32_t max_df = program_.event_max_df[i];
                if (max_df >= 0 && static_cast<std::size_t>(max_df) >= smp.discounts.size())
                    throw InvalidInput("df(): not resolved against this event's own discount lookups -- "
                                       "DiscountLookupResolver must run before this product is priced");
                scripting::run_event(view, program_.event_begin[i], program_.event_begin[i + 1], m,
                                     smp.spots.data(), smp.discounts.data(), smp.numeraire);
            }
            out.assign(1, m.payoff);
        }

        /// Sort-merge the events onto a canonical Time axis. Events sorted by
        /// date already; those within TIMELINE_EPS of each other collapse to
        /// one (statements concatenated in date order — WP §7). An event on
        /// or before the valuation date goes to historical_events_ instead
        /// (WP 16e): it has no future path to attach to.
        void resolve_timeline(std::vector<scripting::Event> events,
                              const ValuationContext &ctx)
        {
            for (scripting::Event &event : events)
            {
                const Time t = ctx.t(event.date);
                if (t <= TIMELINE_EPS)
                {
                    historical_events_.push_back(std::move(event));
                    continue;
                }

                if (!timeline_.empty() && t - timeline_.back() < TIMELINE_EPS)
                {
                    for (scripting::ExprTree &s : event.statements)
                        events_.back().statements.push_back(std::move(s));
                }
                else
                {
                    timeline_.push_back(t);
                    events_.push_back(std::move(event));
                }
            }
        }

        /// Replay every historical event once, in date order, against its
        /// caller-supplied fixing, and return the resulting variable state
        /// -- the baseline every future path starts from instead of zero.
        std::vector<T>
        replay_historical(std::size_t n_variables,
                          const std::map<Date, std::vector<double>> &fixings)
        {
            scripting::Evaluator<T> replay; // always hard (see class doc)
            replay.set_variable_count(n_variables);
            replay.set_suppress_payoff(true);
            replay.initialize();

            Scenario<T> fixing_sample(1);
            for (const scripting::Event &event : historical_events_)
            {
                const auto it = fixings.find(event.date);
                if (it == fixings.end())
                    throw InvalidInput(
                        "ScriptedProduct: missing a historical fixing for " +
                        event.date.to_iso());

                fixing_sample[0].spots.assign(it->second.begin(),
                                              it->second.end());
                fixing_sample[0].numeraire = T(1); // unused: payoff suppressed
                replay.set_event(fixing_sample, 0);
                for (const scripting::ExprTree &statement : event.statements)
                    replay.run(*statement);
            }
            return replay.variables();
        }

        std::vector<scripting::Event> events_;            ///< AST, const after construction
        std::vector<scripting::Event> historical_events_; ///< replayed once, at construction
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::size_t n_underlyings_ = 1;
        scripting::ScriptAnalysis analysis_;
        std::vector<std::string> variable_names_;
        std::vector<std::string> labels_{"price"};
        mutable std::unique_ptr<scripting::Evaluator<T>>
            evaluator_; ///< per-path state (§5.4); hard or fuzzy
        std::vector<T> baseline_;
        scripting::Program program_;
        bool compiled_ = false;
        // The machine's per-path buffers (one product per thread, like evaluator_).
        mutable std::vector<T> vars_, stack_, degrees_, if_slots_;
        mutable std::vector<int> if_mode_;
        bool has_exercise_ = false;
        scripting::ExercisePolicy policy_;
    };

} // namespace quantModeling

#endif // QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP
