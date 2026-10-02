#include "quantModeling/engines/xva/script_future_value.hpp"

#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/models/hybrid/hull_white_equity_sim_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);

        std::size_t grid_index(const std::vector<Time> &grid, Time t)
        {
            // Script dates are calendar dates turned into year fractions: the
            // grid holds them to the timeline's own tolerance.
            const auto it = std::lower_bound(grid.begin(), grid.end(), t - kTimeEps);
            if (it == grid.end() || std::abs(*it - t) > kTimeEps)
                throw InvalidInput("exposure grid is missing the script's event date t=" +
                                   std::to_string(t));
            return static_cast<std::size_t>(it - grid.begin());
        }

        class ScriptFutureValue final : public FutureValue
        {
          public:
            ScriptFutureValue(std::string script, const ValuationContext &ctx,
                              const HullWhiteCurveModel &model, const ScriptExposureSettings &settings)
                : script_(std::move(script)), ctx_(ctx), model_(model), regression_(resolved(settings.regression))
            {
                if (settings.pricing_paths < 1000)
                    throw InvalidInput("script future value: at least 1000 pricing paths");
                ScriptedProduct<Real> product(script_, ctx_);
                events_ = product.timeline();
                defline_ = product.defline();
                variables_ = product.variable_names().size();
                if (events_.empty())
                    throw InvalidInput("script future value: the script has no event after the "
                                       "valuation date");
                if (variables_ + 1 > kMaxRegressors)
                    throw InvalidInput("script future value: the script has " +
                                       std::to_string(variables_) + " variables; the regression of "
                                                                    "its value takes at most " +
                                       std::to_string(kMaxRegressors - 1));
                for (const SampleDef &def : defline_)
                    if (!def.forward_mats.empty())
                        throw InvalidInput("script future value: forward() is not available on the "
                                           "exposure engine, which simulates rates only");

                // The script's own pricing: an equity nobody reads next to
                // the same Hull-White rates. It fits the exercise rule and
                // gives today's value.
                HullWhiteEquitySimModel<Real> pricing(1.0, 0.0, 0.2, model_, 0.0);
                if (product.has_exercise())
                {
                    fit_exercise_policy(product, pricing, settings.pricing_paths, settings.seed);
                    policy_ = product.exercise_policy();
                }
                PricingSettings s;
                s.mc_paths = settings.pricing_paths;
                s.mc_seed = settings.seed;
                today_ = simulate<Real>(product, pricing, s).npv();

                // spot() on a path of rates: found now, not in the middle of
                // a simulation.
                Scratch probe(*this);
                const std::vector<Real> flat(events_.size(), 0.0);
                fill(probe, flat.data(), /*state_is_per_event=*/true);
                probe.product.replay(probe.scenario, probe.cumulative);
                require_finite(probe.cumulative.back());
            }

            std::vector<Time> event_times() const override { return events_; }
            Time maturity() const override { return events_.back(); }
            Real value_today() const override { return today_; }

            void bind(const std::vector<Time> &grid) override
            {
                event_date_.clear();
                for (const Time t : events_)
                    event_date_.push_back(grid_index(grid, t));
                // The last event at or before each grid date.
                last_event_.assign(grid.size(), kNone);
                std::size_t e = 0;
                for (std::size_t i = 0; i < grid.size(); ++i)
                {
                    while (e < events_.size() && event_date_[e] <= i)
                        ++e;
                    last_event_[i] = e == 0 ? kNone : e - 1;
                }
                fits_.assign(grid.size(), Fit{});
                fitted_ = false;
            }

            bool needs_pilot() const override { return true; }

            void fit(const PilotPaths &pilot) override
            {
                const std::size_t n = pilot.dates(), N = pilot.paths, E = events_.size();
                if (n != fits_.size())
                    throw InvalidInput("script future value: the pilot paths are not on the grid");

                // Every pilot path once: its cash flows by date, and its
                // variables after each event.
                std::vector<Real> flows(N * n, 0.0), variables(N * E * variables_);
                Scratch scratch(*this);
                for (std::size_t p = 0; p < N; ++p)
                {
                    run(scratch, pilot.state_row(p));
                    for (std::size_t e = 0; e < E; ++e)
                        flows[p * n + event_date_[e]] +=
                            scratch.cumulative[e] - (e == 0 ? 0.0 : scratch.cumulative[e - 1]);
                    std::copy(scratch.variables.begin(), scratch.variables.end(),
                              variables.begin() + static_cast<std::ptrdiff_t>(p * E * variables_));
                }

                // What each variable is after each event: the same on every
                // path (it says nothing), a state taking a few values (a flag
                // set on exercise: a regime, which no polynomial follows), or
                // a number (a regressor).
                std::vector<std::vector<std::size_t>> flags(E), numbers(E);
                for (std::size_t e = 0; e < E; ++e)
                    for (std::size_t v = 0; v < variables_; ++v)
                    {
                        std::vector<Real> seen;
                        for (std::size_t p = 0; p < N && seen.size() <= kMaxStates; ++p)
                        {
                            const Real x = variables[(p * E + e) * variables_ + v];
                            if (std::find(seen.begin(), seen.end(), x) == seen.end())
                                seen.push_back(x);
                        }
                        if (seen.size() > kMaxStates)
                            numbers[e].push_back(v);
                        else if (seen.size() > 1)
                            flags[e].push_back(v);
                    }

                // Backward: future[p] = Σ_{j>i} flow_j deflator_j.
                std::vector<Real> future(N, 0.0);
                struct Points
                {
                    std::vector<Real> rows, targets;
                };
                std::vector<Real> key;
                for (std::size_t i = n; i-- > 0;)
                {
                    Fit &f = fits_[i];
                    const std::size_t e = last_event_[i];
                    if (e != kNone)
                    {
                        f.flags = flags[e];
                        f.numbers = numbers[e];
                    }
                    // After the last event nothing is left to pay.
                    if (e == kNone || e + 1 < E)
                    {
                        const std::size_t width = 1 + f.numbers.size();
                        std::vector<std::pair<std::vector<Real>, Points>> by_regime;
                        for (std::size_t p = 0; p < N; ++p)
                        {
                            const Real *vars =
                                e == kNone ? nullptr : variables.data() + (p * E + e) * variables_;
                            key.clear();
                            for (const std::size_t v : f.flags)
                                key.push_back(vars[v]);
                            auto it = std::find_if(by_regime.begin(), by_regime.end(),
                                                   [&key](const auto &r)
                                                   { return r.first == key; });
                            if (it == by_regime.end())
                                it = by_regime.insert(by_regime.end(), {key, Points{}});
                            Points &sample = it->second;
                            sample.rows.push_back(pilot.state_row(p)[i]);
                            for (const std::size_t v : f.numbers)
                                sample.rows.push_back(vars[v]);
                            sample.targets.push_back(future[p] / pilot.deflator_row(p)[i]);
                        }
                        for (auto &[regime, sample] : by_regime)
                            f.values.emplace_back(regime, BucketedRegression::fit(sample.rows, width,
                                                                                  sample.targets,
                                                                                  regression_));
                    }
                    for (std::size_t p = 0; p < N; ++p)
                        future[p] += flows[p * n + i] * pilot.deflator_row(p)[i];
                }
                fitted_ = true;
            }

            struct Scratch final : Workspace
            {
                explicit Scratch(const ScriptFutureValue &owner)
                    : product(owner.script_, owner.ctx_)
                {
                    if (!owner.policy_.empty())
                        product.set_exercise_policy(owner.policy_);
                    allocate_scenario(scenario, owner.defline_, product.n_underlyings());
                    for (Sample<Real> &sample : scenario)
                    {
                        sample.initialize();
                        // Nothing here simulates an equity.
                        std::fill(sample.spots.begin(), sample.spots.end(),
                                  std::numeric_limits<Real>::quiet_NaN());
                    }
                }
                ScriptedProduct<Real> product;
                Scenario<Real> scenario;
                std::vector<Real> cumulative, variables;
            };

            std::unique_ptr<Workspace> make_workspace() const override
            {
                return std::make_unique<Scratch>(*this);
            }

            void evaluate_path(const Real *state, std::size_t dates, Real *values, Real *cashflows,
                               Workspace *workspace) const override
            {
                if (!fitted_)
                    throw InvalidInput("script future value: fit() must run before the simulation");
                Scratch &scratch = static_cast<Scratch &>(*workspace);
                run(scratch, state);
                if (cashflows != nullptr)
                {
                    std::fill(cashflows, cashflows + dates, 0.0);
                    for (std::size_t e = 0; e < events_.size(); ++e)
                        cashflows[event_date_[e]] +=
                            scratch.cumulative[e] - (e == 0 ? 0.0 : scratch.cumulative[e - 1]);
                }
                std::array<Real, kMaxRegressors> z{};
                std::array<Real, kMaxRegressors> key{};
                for (std::size_t i = 0; i < dates; ++i)
                {
                    const Fit &f = fits_[i];
                    const std::size_t e = last_event_[i];
                    const Real *vars =
                        e == kNone ? nullptr : scratch.variables.data() + e * variables_;
                    for (std::size_t k = 0; k < f.flags.size(); ++k)
                        key[k] = vars[f.flags[k]];
                    z[0] = state[i];
                    for (std::size_t k = 0; k < f.numbers.size(); ++k)
                        z[1 + k] = vars[f.numbers[k]];
                    // A regime no pilot path was in: nothing to estimate it
                    // from, and it is worth nothing here.
                    values[i] = 0.0;
                    for (const auto &[regime, regression] : f.values)
                        if (std::equal(regime.begin(), regime.end(), key.begin()))
                        {
                            values[i] = regression(z.data());
                            break;
                        }
                }
            }

            // One date at a time: a whole path each call. The engine does not
            // come this way (evaluate_path); a wrapper might.
            Real value(std::size_t i, const Real *state) const override
            {
                return one_date(i, state, false);
            }
            Real cashflow(std::size_t i, const Real *state) const override
            {
                return one_date(i, state, true);
            }

          private:
            /// A variable with at most this many values on the pilot paths is
            /// a state, not a number.
            static constexpr std::size_t kMaxStates = 4;

            struct Fit
            {
                /// The variables that make the regime at this date, and those
                /// that are regressors next to the model state.
                std::vector<std::size_t> flags, numbers;
                /// One regression per regime seen on the pilot paths, keyed
                /// by the values of the flags.
                std::vector<std::pair<std::vector<Real>, BucketedRegression>> values;
            };

            static void require_finite(Real x)
            {
                if (!std::isfinite(x))
                    throw InvalidInput("script future value: the script reads spot(), and the "
                                       "exposure engine simulates rates only");
            }

            /// The scenario of one path: at each event, a numeraire of one
            /// and the discount factors the script asks for, from the state.
            void fill(Scratch &scratch, const Real *state, bool state_is_per_event) const
            {
                for (std::size_t e = 0; e < events_.size(); ++e)
                {
                    const Real x = state[state_is_per_event ? e : event_date_[e]];
                    Sample<Real> &sample = scratch.scenario[e];
                    sample.numeraire = 1.0;
                    const std::vector<Time> &mats = defline_[e].discount_mats;
                    for (std::size_t k = 0; k < mats.size(); ++k)
                        sample.discounts[k] = model_.zcb(events_[e], mats[k], x);
                }
            }

            void run(Scratch &scratch, const Real *state) const
            {
                fill(scratch, state, false);
                scratch.product.replay(scratch.scenario, scratch.cumulative, &scratch.variables);
                require_finite(scratch.cumulative.back());
            }

            Real one_date(std::size_t i, const Real *state, bool flow) const
            {
                // The state is only known up to date i: later events read a
                // copy padded with the last known state, and are not used.
                const std::size_t n = fits_.size();
                std::vector<Real> padded(n, state[i]);
                std::copy(state, state + i + 1, padded.begin());
                std::vector<Real> values(n), flows(n);
                Scratch scratch(*this);
                evaluate_path(padded.data(), n, values.data(), flows.data(), &scratch);
                return flow ? flows[i] : values[i];
            }

            std::string script_;
            ValuationContext ctx_;
            const HullWhiteCurveModel &model_;
            AmcSettings regression_;
            TimeLine events_;
            std::vector<SampleDef> defline_;
            std::size_t variables_ = 0;
            scripting::ExercisePolicy policy_;
            Real today_ = 0.0;
            /// Grid index of each event.
            std::vector<std::size_t> event_date_;
            /// Per grid date: the last event at or before it (kNone: none yet).
            std::vector<std::size_t> last_event_;
            std::vector<Fit> fits_;
            bool fitted_ = false;
        };
    } // namespace

    std::unique_ptr<FutureValue> make_script_future_value(const std::string &script,
                                                          const ValuationContext &ctx,
                                                          const HullWhiteCurveModel &model,
                                                          const ScriptExposureSettings &settings)
    {
        return std::make_unique<ScriptFutureValue>(script, ctx, model, settings);
    }

} // namespace quantModeling
