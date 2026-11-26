#ifndef QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP
#define QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/sample.hpp"
#include <functional>
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/bytecode.hpp"
#include "quantModeling/scripting/ir.hpp"
#include "quantModeling/scripting/compiler.hpp"
#include "quantModeling/scripting/evaluator.hpp"
#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/exercise.hpp"
#include "quantModeling/scripting/fuzzy_evaluator.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/knock_outs.hpp"
#include "quantModeling/scripting/visitors/step_monitor_resolver.hpp"
#include "quantModeling/scripting/visitors/const_cond.hpp"
#include "quantModeling/scripting/visitors/defline_builder.hpp"
#include "quantModeling/scripting/visitors/discount_lookup_resolver.hpp"
#include "quantModeling/scripting/visitors/domain_processor.hpp"
#include "quantModeling/scripting/visitors/if_processor.hpp"
#include "quantModeling/utils/stats.hpp"
#include "quantModeling/scripting/visitors/script_analyzer.hpp"
#include "quantModeling/scripting/visitors/var_indexer.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
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
        /// A controlPays anywhere below this node.
        inline bool contains_control(const scripting::Node &node)
        {
            if (dynamic_cast<const scripting::NodeControlPays *>(&node))
                return true;
            for (const auto &child : node.arguments)
                if (child && contains_control(*child))
                    return true;
            return false;
        }

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
        /// The expectation of the script's `controlPays` cash flows (their
        /// sum deflated, as a price), when the caller knows it in closed
        /// form under the model it prices with (WP 26 P2): the engine then
        /// uses them as a control variate. Without it, controlPays is
        /// computed and ignored.
        std::optional<double> control_mean{};
        /// The numbers written in the script are a product's terms (strike,
        /// barriers, coupons): the GPU's compiled kernel reads them as data
        /// instead of holding them as constants, so that one kernel serves
        /// every set of terms and every number of dates of the same product
        /// shape (blueprint/wp/27-ir-ad.md §6.1, lot L1b). For the scripts
        /// the library writes (pricers/script_builders); a script a user
        /// wrote keeps its numbers as constants. Same bits either way.
        bool terms_as_parameters = false;
        /// knockOutAbove / Below as the thresholds they stand for (#369 L2):
        /// the hard script -- smoothed when `fuzzy` -- for a model that
        /// cannot condition its step, or greeks smoothed at a width.
        bool knock_outs_as_thresholds = false;
        /// With `fuzzy`: a C1 ramp for the smoothed comparisons -- the
        /// clamped linear ramp u replaced by u^2 (3 - 2 u), whose slope is 0
        /// where it meets 0 and 1 -- so that the smoothed payoff has a second
        /// derivative along a path and the second-order risks by tangent over
        /// adjoint are exact for it (blueprint/wp/27-ir-ad.md §8, rung 2).
        /// The linear ramp's corners carry the whole gamma, which a pathwise
        /// second derivative cannot see. Another smoothing, so another
        /// (equally small) bias on the price: off by default.
        bool smooth_ramp = false;
        /// A historical event holding exercise() or call() is a right that
        /// was not used: the holder (or the issuer) would have booked the
        /// exercise as a closing trade. The whole event is then skipped by
        /// the replay instead of refused -- its statements only make that
        /// decision in the scripts this is for (Bermudans, Americans); a
        /// variable it would have set keeps its earlier value. For a
        /// position held in a portfolio (issue 313, lot b3).
        bool past_rights_unexercised = false;
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
            // knockOutAbove / Below (issue #369): the weight variable, its
            // start at 1, and every later payment multiplied by it.
            // A knock-out whose level is not known today (a forward start)
            // cannot be conditioned on: then every knock-out is priced as
            // its threshold, and the analysis says so (#369 L2).
            const auto n_past = static_cast<std::size_t>(
                std::count_if(events.begin(), events.end(), [&](const scripting::Event &e)
                              { return ctx.t(e.date) <= TIMELINE_EPS; }));
            const bool forced_thresholds = !settings.knock_outs_as_thresholds &&
                                           !scripting::knock_out_levels_known_today(events, n_past);
            const bool knock_outs = scripting::prepare_knock_outs(
                events, settings.knock_outs_as_thresholds || forced_thresholds);

            scripting::VarIndexer indexer;
            indexer.index(events);
            variable_names_ = indexer.names();
            for (std::size_t v = 0; v < variable_names_.size(); ++v)
                if (variable_names_[v] == scripting::NodeControlPays::kControlVariable)
                    control_index_ = static_cast<std::ptrdiff_t>(v);
            control_mean_ = settings.control_mean;

            scripting::ConstCondProcessor().process(events);
            scripting::IfProcessor().process(events);
            scripting::DomainProcessor(indexer.count(), settings.default_eps)
                .process(events);

            // Historical events (resolve_timeline's split, below) are known
            // fixings: the analysis reads only what is still to be simulated.
            const auto n_historical = static_cast<std::size_t>(
                std::count_if(events.begin(), events.end(), [&](const scripting::Event &e)
                              { return ctx.t(e.date) <= TIMELINE_EPS; }));
            analysis_ = scripting::analyze_script(events, n_historical);
            analysis_.knock_outs_on_thresholds = forced_thresholds;

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
            const auto holds_right = [](const scripting::Event &event)
            {
                std::vector<const scripting::NodeExercise *> past;
                for (const scripting::ExprTree &statement : event.statements)
                    if (statement)
                        detail::collect_exercises(*statement, past);
                return !past.empty();
            };
            if (settings.past_rights_unexercised)
                std::erase_if(historical_events_, holds_right);
            for (const scripting::Event &event : historical_events_)
                if (holds_right(event))
                    throw InvalidInput("exercise() / call() on a past date (" + event.date.to_iso() +
                                       "): the decision was already taken; script its outcome instead");
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

            if (knock_outs && scripting::has_knock_out(historical_events_))
                throw InvalidInput("knockOutAbove() / knockOutBelow() on a past date: the path up to the "
                                   "valuation date is known; script the barrier on the fixings instead");
            const std::vector<std::vector<Time>> historical_mats =
                scripting::DiscountLookupResolver(ctx).resolve(historical_events_);
            for (std::size_t i = 0; i < historical_mats.size(); ++i)
                if (!historical_mats[i].empty())
                    throw InvalidInput(
                        "ScriptedProduct: df() is not supported in a "
                        "historical (pre-valuation) event (" +
                        historical_events_[i].date.to_iso() + ")");
            const std::vector<std::vector<Time>> historical_forwards =
                scripting::ForwardLookupResolver(ctx).resolve(historical_events_);
            for (std::size_t i = 0; i < historical_forwards.size(); ++i)
                if (!historical_forwards[i].empty())
                    throw InvalidInput("ScriptedProduct: fwd() is not supported in a historical (pre-valuation) "
                                       "event (" +
                                       historical_events_[i].date.to_iso() + ")");
            const std::vector<std::vector<StepMonitor>> historical_barriers =
                scripting::StepMonitorResolver().resolve(historical_events_);
            for (std::size_t i = 0; i < historical_barriers.size(); ++i)
                if (!historical_barriers[i].empty())
                    throw InvalidInput("ScriptedProduct: survivalAbove() / survivalBelow() on a past date (" +
                                       historical_events_[i].date.to_iso() +
                                       "): the path between past fixings is not known; script the barrier "
                                       "on the fixings instead");

            // The historical fixings first: a knock-out's level may read
            // them (`barrier * s0`, s0 struck on a past date).
            if (!historical_events_.empty())
                baseline_ = replay_historical(indexer.count(), historical_fixings);
            if (knock_outs)
            {
                std::vector<double> known(baseline_.size());
                for (std::size_t v = 0; v < known.size(); ++v)
                    known[v] = to_double(baseline_[v]);
                scripting::resolve_knock_out_levels(events_, known, indexer.count());
            }

            const std::vector<std::vector<Time>> discount_mats =
                scripting::DiscountLookupResolver(ctx).resolve(events_);
            const std::vector<std::vector<StepMonitor>> barriers =
                scripting::StepMonitorResolver().resolve(events_);
            const std::vector<std::vector<Time>> forward_mats =
                scripting::ForwardLookupResolver(ctx).resolve(events_);
            defline_ = scripting::build_defline(events_, discount_mats, barriers, forward_mats);

            if (settings.fuzzy)
            {
                auto fuzzy = std::make_unique<scripting::FuzzyEvaluator<T>>();
                fuzzy->set_smooth_ramp(settings.smooth_ramp);
                evaluator_ = std::move(fuzzy);
            }
            else
                evaluator_ = std::make_unique<scripting::Evaluator<T>>();
            evaluator_->set_variable_count(indexer.count());

            if (!historical_events_.empty())
                evaluator_->set_baseline(baseline_);

            // Early exercise compiles too (WP 26 §15): the bytecode decides
            // with the fitted rule, the pilot stays on the tree evaluator. It
            // has no IR, hence no GPU kernel; with more regressors than the
            // machine holds it stays on the tree evaluator altogether.
            const std::size_t regressors =
                !has_exercise_ ? 0
                               : (exercises.front()->arguments.empty() ? n_underlyings_
                                                                       : exercises.front()->arguments.size());
            exercise_regressors_ = regressors;
            if (settings.bytecode && regressors <= static_cast<std::size_t>(scripting::kMaxExerciseRegressors))
            {
                program_ = scripting::compile_script(events_, indexer.count(), settings.fuzzy, settings.smooth_ramp,
                                                     n_underlyings_);
                if (!has_exercise_)
                {
                    // What the IR cannot follow (fwd()) leaves it empty, the
                    // reason kept: the CPU prices it.
                    try
                    {
                        ir_ = scripting::ir::lower_script(events_, indexer.count(), settings.fuzzy,
                                                          settings.terms_as_parameters, settings.smooth_ramp);
                    }
                    catch (const InvalidInput &e)
                    {
                        ir_ = {};
                        ir_refusal_ = e.what();
                    }
                }
                else
                    ir_refusal_ = "early exercise: the IR is lowered once the exercise rule is fitted";
                ir_settings_ = settings;
                ir_n_vars_ = indexer.count();
                buffers_.vars.assign(static_cast<std::size_t>(program_.n_vars), T(0));
                buffers_.stack.assign(static_cast<std::size_t>(program_.max_stack), T(0));
                buffers_.degrees.assign(static_cast<std::size_t>(program_.max_degrees), T(0));
                buffers_.if_slots.assign(static_cast<std::size_t>(program_.n_if_slots), T(0));
                buffers_.if_mode.assign(static_cast<std::size_t>(program_.n_if_modes), 0);
                compiled_ = true;
                batchable_ = program_.batchable();
            }
        }

        /// The script's controlPays cash flows as a control variate, when
        /// ScriptSettings::control_mean gave their expectation.
        bool analytic_control(Real &mean) const override
        {
            if (control_index_ < 0 || !control_mean_)
                return false;
            mean = *control_mean_;
            return true;
        }

        /// The controlPays variable's slot, -1 without one.
        std::ptrdiff_t control_variable() const { return control_index_; }

        /// The expectation of the script's controlPays cash flows given from
        /// outside (ScriptSettings::control_mean, or the engine's own closed
        /// form below); nullopt turns the control off.
        void set_control_mean(std::optional<double> mean) { control_mean_ = mean; }

        /**
         * @brief The discounted expectation of the controlPays cash flows
         *        under Black-Scholes, when every one of them is a closed form
         *        the engine knows (WP 26 P2, the maintainer's choice of
         *        2026-11-16 for the scripting page): a vanilla call or put on
         *        one asset's spot, that spot itself (a forward), a fixed
         *        amount -- each possibly times a constant, and sums of them --
         *        paid at an event's date, at the top level of the event (not
         *        inside an if). nullopt when there is no controlPays, or one
         *        it does not recognise: the control is then left off.
         *
         * @param spots, dividends, vols one per asset (spot(i)); the rate is
         *        flat, and the numeraire exp(r t), as BlackScholesSimModel.
         */
        std::optional<double> black_scholes_control_mean(const std::vector<Real> &spots, Real rate,
                                                         const std::vector<Real> &dividends,
                                                         const std::vector<Real> &vols) const
        {
            if (control_index_ < 0)
                return std::nullopt;
            using namespace scripting;
            const auto constant = [](const Node &n, double &x)
            {
                if (const auto *c = dynamic_cast<const NodeConst *>(&n))
                {
                    x = c->value;
                    return true;
                }
                return false;
            };
            // E[amount] at time t (undiscounted), or nullopt.
            std::function<std::optional<double>(const Node &, Time)> forward_value =
                [&](const Node &n, Time t) -> std::optional<double>
            {
                double x = 0.0;
                if (constant(n, x))
                    return x;
                const auto asset_of = [&](const NodeSpot &sp) -> std::optional<std::size_t>
                {
                    if (sp.index >= spots.size() || sp.index >= vols.size())
                        return std::nullopt;
                    return sp.index;
                };
                const auto fwd = [&](std::size_t a)
                { return spots[a] * std::exp((rate - (a < dividends.size() ? dividends[a] : 0.0)) * t); };
                if (const auto *sp = dynamic_cast<const NodeSpot *>(&n))
                {
                    const auto a = asset_of(*sp);
                    return a ? std::optional<double>(fwd(*a)) : std::nullopt;
                }
                if (dynamic_cast<const NodeAdd *>(&n) && n.arguments.size() == 2)
                {
                    const auto l = forward_value(*n.arguments[0], t), r = forward_value(*n.arguments[1], t);
                    return l && r ? std::optional<double>(*l + *r) : std::nullopt;
                }
                if (dynamic_cast<const NodeMult *>(&n) && n.arguments.size() == 2)
                {
                    if (constant(*n.arguments[0], x))
                    {
                        const auto r = forward_value(*n.arguments[1], t);
                        return r ? std::optional<double>(x * *r) : std::nullopt;
                    }
                    if (constant(*n.arguments[1], x))
                    {
                        const auto l = forward_value(*n.arguments[0], t);
                        return l ? std::optional<double>(x * *l) : std::nullopt;
                    }
                    return std::nullopt;
                }
                // max(spot(i) - K, 0) and max(K - spot(i), 0): Black's formula.
                if (dynamic_cast<const NodeMax *>(&n) && n.arguments.size() == 2)
                {
                    double zero = 1.0;
                    if (!constant(*n.arguments[1], zero) || zero != 0.0)
                        return std::nullopt;
                    const auto *diff = dynamic_cast<const NodeSub *>(n.arguments[0].get());
                    if (!diff || diff->arguments.size() != 2)
                        return std::nullopt;
                    double K = 0.0;
                    const NodeSpot *sp = nullptr;
                    bool call = false;
                    if ((sp = dynamic_cast<const NodeSpot *>(diff->arguments[0].get())) &&
                        constant(*diff->arguments[1], K))
                        call = true;
                    else if (!(constant(*diff->arguments[0], K) &&
                               (sp = dynamic_cast<const NodeSpot *>(diff->arguments[1].get()))))
                        return std::nullopt;
                    const auto a = asset_of(*sp);
                    if (!a || !(K > 0.0) || !(t > 0.0))
                        return std::nullopt;
                    const double F = fwd(*a), sd = vols[*a] * std::sqrt(t);
                    if (!(sd > 0.0))
                        return call ? std::max(F - K, 0.0) : std::max(K - F, 0.0);
                    const double d1 = (std::log(F / K) + 0.5 * sd * sd) / sd, d2 = d1 - sd;
                    return call ? F * norm_cdf(d1) - K * norm_cdf(d2) : K * norm_cdf(-d2) - F * norm_cdf(-d1);
                }
                return std::nullopt;
            };
            double mean = 0.0;
            bool any = false;
            for (std::size_t e = 0; e < events_.size(); ++e)
            {
                const Time t = timeline_[e];
                for (const ExprTree &statement : events_[e].statements)
                {
                    if (!statement)
                        continue;
                    // The parser wraps each statement in a NodeCollect.
                    const Node *node = statement.get();
                    while (dynamic_cast<const NodeCollect *>(node) && node->arguments.size() == 1 &&
                           node->arguments[0])
                        node = node->arguments[0].get();
                    const auto *top = dynamic_cast<const NodeControlPays *>(node);
                    if (!top)
                    {
                        // a controlPays inside an if: no closed form here
                        if (quantModeling::detail::contains_control(*statement))
                            return std::nullopt;
                        continue;
                    }
                    const auto v = forward_value(*top->arguments[1], t);
                    if (!v)
                        return std::nullopt;
                    mean += std::exp(-rate * t) * *v;
                    any = true;
                }
            }
            return any ? std::optional<double>(mean) : std::nullopt;
        }

        Real control_value(const typename ISimulatableProduct<T>::PayoffState *state) const override
        {
            if (control_index_ < 0)
                return 0.0;
            const auto v = static_cast<std::size_t>(control_index_);
            if (!runs_bytecode())
                return static_cast<Real>(
                    (state ? *static_cast<const TreeState *>(state)->evaluator : *evaluator_).variables()[v]);
            const auto *b = state ? static_cast<const MachineBuffers *>(state) : &buffers_;
            return static_cast<Real>(b->vars[v]);
        }

        /// The script has exercise() or call(): it prices only once an
        /// exercise rule is set (fit_exercise_policy, engines/mc/lsm.hpp).
        bool has_exercise() const { return has_exercise_; }
        bool issuer_exercise() const { return policy_.issuer; }

        /// The exercise rule payoffs() applies from now on. A compiled
        /// script then prices through the bytecode, whose Exercise
        /// instruction reads the rule as flat data.
        void set_exercise_policy(scripting::ExercisePolicy policy)
        {
            policy_ = std::move(policy);
            evaluator_->set_exercise_policy(&policy_);
            flatten_policy();
            policy_set_ = true;
            lower_with_policy();
        }

        /// Why the script has no IR (hence no GPU kernel), empty when it has
        /// one.
        const std::string &ir_refusal() const { return ir_refusal_; }

        /// The rule set by set_exercise_policy, as the bytecode reads it.
        scripting::ExerciseView exercise_rule() const { return exercise_view(); }
        const scripting::ExercisePolicy &exercise_policy() const { return policy_; }

        /**
         * @brief The pilot's view of one path: the script run once without
         *        exercising, then once per exercise date it reached with the
         *        right exercised there. Leaves the product under its rule.
         */
        ExercisePilotPath exercise_pilot(const Scenario<T> &path) const
        {
            if (compiled_)
                return exercise_pilot_compiled(path, buffers_, pilot_);
            return exercise_pilot_tree(path, *evaluator_);
        }

        /// The compiled script (empty when ScriptSettings::bytecode is off).
        const scripting::Program &program() const { return program_; }
        /// The script's intermediate representation (blueprint/wp/27-ir-ad.md):
        /// what the GPU compiles, and what the derivatives are built from.
        /// Empty when the bytecode is.
        const scripting::ir::Script &ir() const { return ir_; }
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
        /// What the historical events' `pays` added up to, undiscounted: the
        /// cash flows a seasoned product already paid (not in today's price).
        double historical_payments() const { return historical_payments_; }

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
            if (runs_bytecode())
                return payoffs_compiled(path, out, buffers_);
            out.assign(1, run_tree(path));
        }

        /// The bytecode machine's per-path buffers.
        struct MachineBuffers final : ISimulatableProduct<T>::PayoffState
        {
            std::vector<T> vars, stack, degrees, if_slots;
            std::vector<int> if_mode;
        };

        /// The compiled pilot's scratch: the variables and the payoff at the
        /// start of each event, and the decisions the never-exercised run
        /// reached.
        struct PilotScratch
        {
            std::vector<T> vars, payoffs;
            std::vector<std::int32_t> log_events;
            std::vector<double> log_regressors;
        };

        /// What one thread of the pilot needs (fit_exercise_policy on a
        /// pool): the machine's buffers and scratch, or its own copy of the
        /// tree evaluator.
        struct PilotState
        {
            MachineBuffers buffers;
            PilotScratch scratch;
            std::unique_ptr<scripting::Evaluator<T>> evaluator;
        };

        std::unique_ptr<PilotState> make_pilot_state() const
        {
            auto state = std::make_unique<PilotState>();
            if (compiled_)
                state->buffers = buffers_;
            else if (const auto *fuzzy = dynamic_cast<const scripting::FuzzyEvaluator<T> *>(evaluator_.get()))
                state->evaluator = std::make_unique<scripting::FuzzyEvaluator<T>>(*fuzzy);
            else
                state->evaluator = std::make_unique<scripting::Evaluator<T>>(*evaluator_);
            return state;
        }

        /// exercise_pilot on a thread's own state: the same values.
        ExercisePilotPath exercise_pilot(const Scenario<T> &path, PilotState &state) const
        {
            if (compiled_)
                return exercise_pilot_compiled(path, state.buffers, state.scratch);
            return exercise_pilot_tree(path, *state.evaluator);
        }

        /// The tree evaluator's per-thread state (bytecode off, or an early
        /// exercise the bytecode does not take): a copy of the product's evaluator -- its variables,
        /// baseline and settings, and the exercise rule it points to, which
        /// pricing only reads. The same evaluations as payoffs(), so the
        /// same bits on any number of threads.
        struct TreeState final : ISimulatableProduct<T>::PayoffState
        {
            std::unique_ptr<scripting::Evaluator<T>> evaluator;
        };

        /// Concurrent payoffs: on the bytecode path (hard or fuzzy) the state
        /// is five buffers; on the tree evaluator, a copy of it. For a script
        /// with exercise(), made once the exercise rule is set -- the engines
        /// ask for it after fit_exercise_policy -- and used under that rule:
        /// the state's kind follows runs_bytecode().
        std::unique_ptr<typename ISimulatableProduct<T>::PayoffState> make_payoff_state() const override
        {
            if (runs_bytecode())
                return std::make_unique<MachineBuffers>(buffers_);
            auto state = std::make_unique<TreeState>();
            if (const auto *fuzzy = dynamic_cast<const scripting::FuzzyEvaluator<T> *>(evaluator_.get()))
                state->evaluator = std::make_unique<scripting::FuzzyEvaluator<T>>(*fuzzy);
            else
                state->evaluator = std::make_unique<scripting::Evaluator<T>>(*evaluator_);
            state->evaluator->record_exercises(nullptr);
            return state;
        }

        void payoffs_with_state(const Scenario<T> &path, std::vector<T> &out,
                                typename ISimulatableProduct<T>::PayoffState &state) const override
        {
            if (!runs_bytecode())
            {
                out.assign(1, run_tree(path, *static_cast<TreeState &>(state).evaluator));
                return;
            }
            payoffs_compiled(path, out, static_cast<MachineBuffers &>(state));
        }

        /// The batched interpreter's buffers (P1a): kBatch lanes of
        /// variables, stack, and for a fuzzy script degrees and if slots.
        struct BatchBuffers final : ISimulatableProduct<T>::PayoffState
        {
            std::vector<double> vars, stack, degrees, if_slots;
        };

        /// kBatch for a compiled script (hard or fuzzy) priced in double;
        /// 0 otherwise (the tree evaluator, early exercise before its rule
        /// is set, AAD).
        int batch_width() const override
        {
            if constexpr (std::is_same_v<T, double>)
                return runs_bytecode() && batchable_ ? scripting::kBatch : 0;
            else
                return 0;
        }

        std::unique_ptr<typename ISimulatableProduct<T>::PayoffState> make_batch_state() const override
        {
            if (batch_width() == 0)
                return nullptr;
            auto b = std::make_unique<BatchBuffers>();
            b->vars.assign(static_cast<std::size_t>(program_.n_vars) * scripting::kBatch, 0.0);
            b->stack.assign(static_cast<std::size_t>(std::max(1, program_.max_stack)) * scripting::kBatch, 0.0);
            b->degrees.assign(static_cast<std::size_t>(std::max(1, program_.max_degrees)) * scripting::kBatch, 0.0);
            b->if_slots.assign(static_cast<std::size_t>(std::max(1, program_.n_if_slots)) * scripting::kBatch, 0.0);
            return b;
        }

        /**
         * @brief payoffs() for up to kBatch paths at once: each event's
         *        bytecode decoded once for all of them (WP 26 §5, P1a), the
         *        values of payoffs() bit for bit.
         */
        void payoffs_batch(const Scenario<T> *const *paths, int count, T *out,
                           typename ISimulatableProduct<T>::PayoffState &state) const override
        {
            if constexpr (std::is_same_v<T, double>)
            {
                constexpr int W = scripting::kBatch;
                auto &b = static_cast<BatchBuffers &>(state);
                for (int v = 0; v < program_.n_vars; ++v)
                    for (int l = 0; l < W; ++l)
                        b.vars[static_cast<std::size_t>(v * W + l)] =
                            baseline_.empty() ? 0.0 : baseline_[static_cast<std::size_t>(v)];
                scripting::BatchMachine m{b.vars.data(), b.stack.data(), b.degrees.data(), b.if_slots.data(), {}};
                const scripting::ProgramView view = program_.view();
                const scripting::ExerciseView rule = exercise_view();
                if (has_exercise_)
                    m.exercise = &rule;
                const unsigned lanes = count >= W ? (1u << W) - 1u : (1u << count) - 1u;
                const double *spots[W];
                const double *discounts[W];
                const double *survivals[W];
                const double *forwards[W];
                double numeraire[W];
                for (std::size_t i = 0; i < events_.size(); ++i)
                {
                    for (int l = 0; l < W; ++l)
                    {
                        const Sample<T> &smp = (*paths[l < count ? l : 0])[i];
                        spots[l] = smp.spots.data();
                        discounts[l] = smp.discounts.data();
                        survivals[l] = smp.monitored.data();
                        forwards[l] = smp.forwards.data();
                        numeraire[l] = smp.numeraire;
                    }
                    check_event_sizes((*paths[0])[i], i);
                    scripting::run_event_batch(view, program_.event_begin[i], program_.event_begin[i + 1], m, spots,
                                               discounts, numeraire, lanes, survivals, forwards);
                }
                if (m.exercise_twice)
                    throw InvalidInput("exercise() / call() is reached twice in one event: one decision per date");
                for (int l = 0; l < count; ++l)
                    out[l] = m.payoff[l];
            }
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
        T run_tree(const Scenario<T> &path) const { return run_tree(path, *evaluator_); }

        T run_tree(const Scenario<T> &path, scripting::Evaluator<T> &evaluator) const
        {
            evaluator.initialize();
            for (std::size_t i = 0; i < events_.size(); ++i)
                run_event(path, i, evaluator);
            return evaluator.payoff();
        }

        void run_event(const Scenario<T> &path, std::size_t i) const { run_event(path, i, *evaluator_); }

        void run_event(const Scenario<T> &path, std::size_t i, scripting::Evaluator<T> &evaluator) const
        {
            evaluator.set_event(path, i);
            for (const scripting::ExprTree &statement : events_[i].statements)
                evaluator.run(*statement);
        }

        /// Whether a path runs through the bytecode: a compiled script, and
        /// for one with early exercise, once its rule is set (before that
        /// the tree evaluator refuses it, saying what to call).
        bool runs_bytecode() const { return compiled_ && (!has_exercise_ || policy_set_); }

        /// exercise_pilot through the bytecode (WP 26 §15): the tree
        /// evaluator's runs, event for event -- the never-exercised run
        /// logging the decisions it reaches and keeping the variables and
        /// the payoff at the start of every event, then one run per decision
        /// restarted there with the right exercised. The same values, to the
        /// bit.
        ExercisePilotPath exercise_pilot_compiled(const Scenario<T> &path, MachineBuffers &b, PilotScratch &w) const
        {
            ExercisePilotPath out;
            const std::size_t n_events = events_.size(), n_vars = static_cast<std::size_t>(program_.n_vars);
            w.vars.resize(n_events * n_vars);
            w.payoffs.resize(n_events);
            w.log_events.resize(n_events + 1);
            w.log_regressors.resize((n_events + 1) * static_cast<std::size_t>(scripting::kMaxExerciseRegressors));
            if (baseline_.empty())
                std::fill(b.vars.begin(), b.vars.end(), T(0));
            else
                b.vars = baseline_;
            scripting::Machine<T> m{b.vars.data(), b.stack.data(), b.degrees.data(), b.if_slots.data(),
                                    b.if_mode.data(), T(0)};
            const scripting::ProgramView view = program_.view();
            const auto run = [&](std::size_t i)
            {
                const Sample<T> &smp = path[i];
                check_event_sizes(smp, i);
                scripting::run_event(view, program_.event_begin[i], program_.event_begin[i + 1], m,
                                     smp.spots.data(), smp.discounts.data(), smp.numeraire, smp.monitored.data(),
                                     smp.forwards.data());
                if (m.exercise_twice)
                    throw InvalidInput("exercise() / call() is reached twice in one event: one decision per date");
            };
            scripting::ExerciseLog log{w.log_events.data(), w.log_regressors.data(), 0};
            m.exercise_mode = scripting::ExerciseMode::Never;
            m.exercise_log = &log;
            for (std::size_t i = 0; i < n_events; ++i)
            {
                std::copy(b.vars.begin(), b.vars.end(), w.vars.begin() + static_cast<std::ptrdiff_t>(i * n_vars));
                w.payoffs[i] = m.payoff;
                run(i);
            }
            out.never = static_cast<double>(m.payoff);
            m.exercise_log = nullptr;
            m.exercise_mode = scripting::ExerciseMode::AtEvent;
            out.opportunities.reserve(static_cast<std::size_t>(log.count));
            for (std::int32_t k = 0; k < log.count; ++k)
            {
                const auto event = static_cast<std::size_t>(log.events[k]);
                std::copy_n(w.vars.begin() + static_cast<std::ptrdiff_t>(event * n_vars), n_vars, b.vars.begin());
                m.payoff = w.payoffs[event];
                m.exercised = false;
                m.exercise_event = -1;
                m.exercise_at = log.events[k];
                for (std::size_t i = event; i < n_events; ++i)
                    run(i);
                const double *r = w.log_regressors.data() +
                                  static_cast<std::size_t>(k) * static_cast<std::size_t>(scripting::kMaxExerciseRegressors);
                out.opportunities.push_back(
                    {event, std::vector<double>(r, r + exercise_regressors_), static_cast<double>(m.payoff)});
            }
            return out;
        }

        /// The pilot on the tree evaluator `ev`: the script run once without
        /// exercising, keeping the state at the start of every event (a
        /// branch exercised at event e is the same path up to e, so it
        /// restarts there), then once per decision reached. Leaves `ev`
        /// under the product's rule.
        ExercisePilotPath exercise_pilot_tree(const Scenario<T> &path, scripting::Evaluator<T> &ev) const
        {
            using Eval = scripting::Evaluator<T>;
            ExercisePilotPath out;
            std::vector<typename Eval::ExerciseObservation> seen;
            std::vector<typename Eval::State> at_start;
            at_start.reserve(events_.size());
            ev.set_exercise_never();
            ev.record_exercises(&seen);
            ev.initialize();
            for (std::size_t i = 0; i < events_.size(); ++i)
            {
                at_start.push_back(ev.state());
                run_event(path, i, ev);
            }
            out.never = static_cast<double>(ev.payoff());
            ev.record_exercises(nullptr);
            for (auto &obs : seen)
            {
                ev.set_exercise_at(obs.event);
                ev.restore(at_start[obs.event]);
                for (std::size_t i = obs.event; i < events_.size(); ++i)
                    run_event(path, i, ev);
                out.opportunities.push_back(
                    {obs.event, std::move(obs.regressors), static_cast<double>(ev.payoff())});
            }
            ev.set_exercise_policy(policy_.empty() ? nullptr : &policy_);
            return out;
        }

        /// policy_ as the flat data the Exercise instruction reads
        /// (scripting::ExerciseView), one rule per event.
        void flatten_policy()
        {
            exercise_rules_.assign(events_.size(), scripting::ExerciseRule{});
            exercise_data_.clear();
            exercise_exponents_.clear();
            if (!compiled_)
                return;
            for (const auto &[event, reg] : policy_.by_event)
            {
                if (event >= events_.size())
                    throw InvalidInput("exercise rule: an event beyond the script's timeline");
                const std::size_t n = reg.mean.size();
                if (n != exercise_regressors_)
                    throw InvalidInput("exercise(): the number of regressors changed between the pilot and the pricing");
                if (reg.scale.size() != n ||
                    reg.hi.size() != reg.lo.size() || reg.beta.size() != reg.terms.size())
                    throw InvalidInput("exercise rule: inconsistent regression at event " + std::to_string(event));
                scripting::ExerciseRule &rule = exercise_rules_[event];
                rule.first = static_cast<std::int32_t>(exercise_data_.size());
                rule.first_exponent = static_cast<std::int32_t>(exercise_exponents_.size());
                rule.n = static_cast<std::int32_t>(n);
                rule.n_box = static_cast<std::int32_t>(std::min(reg.lo.size(), n));
                rule.n_terms = static_cast<std::int32_t>(reg.terms.size());
                const auto append = [&](const std::vector<double> &v, std::size_t count)
                { exercise_data_.insert(exercise_data_.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(count)); };
                append(reg.mean, n);
                append(reg.scale, n);
                append(reg.lo, static_cast<std::size_t>(rule.n_box));
                append(reg.hi, static_cast<std::size_t>(rule.n_box));
                append(reg.beta, reg.beta.size());
                for (const std::vector<int> &term : reg.terms)
                {
                    if (term.size() != n)
                        throw InvalidInput("exercise rule: inconsistent monomials at event " + std::to_string(event));
                    exercise_exponents_.insert(exercise_exponents_.end(), term.begin(), term.end());
                }
            }
        }

        /// The IR of a script with exercise() under the rule just set
        /// (WP 26 §16): its numbers are the events' terms. What the IR cannot
        /// follow leaves it empty, the reason kept: the CPU prices it.
        void lower_with_policy()
        {
            if (!compiled_ || !has_exercise_)
                return;
            ir_ = {};
            ir_refusal_.clear();
            scripting::ir::ExerciseLowering ex;
            ex.policy = &policy_;
            ex.n_regressors = exercise_regressors_;
            ex.n_spots = n_underlyings_;
            try
            {
                ir_ = scripting::ir::lower_script(events_, ir_n_vars_, ir_settings_.fuzzy,
                                                  ir_settings_.terms_as_parameters, ir_settings_.smooth_ramp, &ex);
            }
            catch (const InvalidInput &e)
            {
                ir_ = {};
                ir_refusal_ = e.what();
            }
        }

        scripting::ExerciseView exercise_view() const
        {
            return {exercise_rules_.data(), exercise_data_.data(), exercise_exponents_.data(), policy_.issuer};
        }

        /// The bytecode path: the tree evaluator's semantics, including its
        /// run-time checks on what the model supplies.
        /// The sample of event i carries what the event's bytecode reads.
        void check_event_sizes(const Sample<T> &smp, std::size_t i) const
        {
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
            const std::int32_t max_fwd = program_.event_max_fwd[i];
            if (max_fwd >= 0 && static_cast<std::size_t>(max_fwd) >= smp.forwards.size())
                throw InvalidInput("fwd(): the model did not fill this event's forwards");
            const std::int32_t max_surv = program_.event_max_surv[i];
            if (max_surv >= 0 && static_cast<std::size_t>(max_surv) >= smp.monitored.size())
                throw InvalidInput("survivalAbove/Below(): the model did not fill this event's survival "
                                   "probabilities -- it must support barrier monitoring");
        }

        void payoffs_compiled(const Scenario<T> &path, std::vector<T> &out, MachineBuffers &b) const
        {
            if (baseline_.empty())
                std::fill(b.vars.begin(), b.vars.end(), T(0));
            else
                b.vars = baseline_;
            scripting::Machine<T> m{b.vars.data(), b.stack.data(), b.degrees.data(), b.if_slots.data(),
                                    b.if_mode.data(), T(0)};
            const scripting::ProgramView view = program_.view();
            const scripting::ExerciseView rule = exercise_view();
            if (has_exercise_)
                m.exercise = &rule;
            for (std::size_t i = 0; i < events_.size(); ++i)
            {
                const Sample<T> &smp = path[i];
                check_event_sizes(smp, i);
                scripting::run_event(view, program_.event_begin[i], program_.event_begin[i + 1], m,
                                     smp.spots.data(), smp.discounts.data(), smp.numeraire, smp.monitored.data(),
                                     smp.forwards.data());
            }
            if (m.exercise_twice)
                throw InvalidInput("exercise() / call() is reached twice in one event: one decision per date");
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
                fixing_sample[0].numeraire = T(1); // the flows are kept undiscounted
                replay.set_event(fixing_sample, 0);
                for (const scripting::ExprTree &statement : event.statements)
                    replay.run(*statement);
            }
            historical_payments_ = to_double(replay.suppressed_payoff());
            return replay.variables();
        }

        std::vector<scripting::Event> events_;            ///< AST, const after construction
        std::vector<scripting::Event> historical_events_; ///< replayed once, at construction
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::size_t n_underlyings_ = 1;
        scripting::ScriptAnalysis analysis_;
        std::vector<std::string> variable_names_;
        std::ptrdiff_t control_index_ = -1; ///< the controlPays variable, -1 without
        std::optional<double> control_mean_;
        std::vector<std::string> labels_{"price"};
        mutable std::unique_ptr<scripting::Evaluator<T>>
            evaluator_; ///< per-path state (§5.4); hard or fuzzy
        std::vector<T> baseline_;
        double historical_payments_ = 0.0;
        scripting::Program program_;
        scripting::ir::Script ir_;
        bool compiled_ = false;
        bool batchable_ = false; ///< run_event_batch can run it (P1a)
        // The machine's per-path buffers for payoffs(); payoffs_with_state()
        // runs on a thread's own copy (make_payoff_state()).
        mutable MachineBuffers buffers_;
        mutable PilotScratch pilot_;
        bool has_exercise_ = false;
        scripting::ExercisePolicy policy_;
        bool policy_set_ = false;
        std::size_t exercise_regressors_ = 0; ///< of each exercise() / call()
        ScriptSettings ir_settings_;          ///< what the IR is lowered with (again once a rule is set)
        std::size_t ir_n_vars_ = 0;
        std::string ir_refusal_;
        // policy_ flattened for the bytecode (flatten_policy)
        std::vector<scripting::ExerciseRule> exercise_rules_;
        std::vector<double> exercise_data_;
        std::vector<std::int32_t> exercise_exponents_;
    };

} // namespace quantModeling

#endif // QM_INSTRUMENTS_SCRIPTED_PRODUCT_HPP
