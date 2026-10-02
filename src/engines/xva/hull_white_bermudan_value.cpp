#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;
        constexpr std::size_t kNever = static_cast<std::size_t>(-1);

        std::size_t grid_index(const std::vector<Time> &grid, Time t)
        {
            const auto it = std::lower_bound(grid.begin(), grid.end(), t - kTimeEps);
            if (it == grid.end() || std::abs(*it - t) > kTimeEps)
                throw InvalidInput("exposure grid is missing the exercise date t=" + std::to_string(t));
            return static_cast<std::size_t>(it - grid.begin());
        }

        class HullWhiteBermudanValue final : public FutureValue
        {
          public:
            HullWhiteBermudanValue(const BermudanSwaption &bermudan, const HullWhiteCurveModel &model,
                                   const AmcSettings &settings)
                : today_(hull_white_bermudan_swaption(bermudan, model)),
                  exercise_times_(bermudan.exercise_times),
                  settings_(resolved(settings)),
                  maturity_(bermudan.swap.maturity())
            {
                if (exercise_times_.empty())
                    throw InvalidInput("Bermudan future value: at least one exercise date is required");
                Time previous = 0.0;
                for (const Time e : exercise_times_)
                {
                    if (!(e > previous + kTimeEps))
                        throw InvalidInput("Bermudan future value: exercise dates must be in the "
                                           "future and strictly increasing");
                    previous = e;
                    const InterestRateSwap tail = bermudan.swap.tail_from(e);
                    if (tail.fixed_leg.empty() || tail.floating_leg.empty())
                        throw InvalidInput("Bermudan future value: no coupon starts on or after the "
                                           "exercise date t=" +
                                           std::to_string(e));
                    // What exercising at e enters into: an exact future value.
                    tails_.push_back(make_future_value(tail, model));
                }
            }

            std::vector<Time> event_times() const override
            {
                std::vector<Time> times = exercise_times_;
                for (const auto &tail : tails_)
                {
                    const std::vector<Time> events = tail->event_times();
                    times.insert(times.end(), events.begin(), events.end());
                }
                return times;
            }

            Time maturity() const override { return maturity_; }

            Real value_today() const override { return today_; }

            void bind(const std::vector<Time> &grid) override
            {
                exercise_index_.clear();
                for (std::size_t k = 0; k < tails_.size(); ++k)
                {
                    tails_[k]->bind(grid);
                    exercise_index_.push_back(grid_index(grid, exercise_times_[k]));
                }
                continuation_.assign(grid.size(), BucketedRegression{});
                fitted_ = false;
            }

            bool needs_pilot() const override { return true; }

            void fit(const PilotPaths &pilot) override
            {
                const std::size_t n = pilot.dates();
                const std::size_t N = pilot.paths;
                if (n != continuation_.size())
                    throw InvalidInput("Bermudan future value: the pilot paths are not on the grid");

                // held[p]: on path p, the deflated value of what a holder who
                // has not exercised up to t_i goes on to receive under the
                // rule fitted so far — the exercise value of the first later
                // date at which the rule exercises, 0 if it never does.
                // Backward in time (Longstaff & Schwartz).
                std::vector<Real> held(N, 0.0);
                std::vector<Real> rows(N), targets(N);
                std::size_t next = tails_.size(); // exercise dates after t_i start here
                for (std::size_t i = n; i-- > 0;)
                {
                    const bool exercise_date = next > 0 && exercise_index_[next - 1] == i;
                    // After the last exercise date nothing is left to hold.
                    if (i < exercise_index_.back())
                    {
                        for (std::size_t p = 0; p < N; ++p)
                        {
                            rows[p] = pilot.state_row(p)[i];
                            targets[p] = held[p] / pilot.deflator_row(p)[i];
                        }
                        continuation_[i] = BucketedRegression::fit(rows, 1, targets, settings_);
                    }
                    if (exercise_date)
                    {
                        const std::size_t k = --next;
                        for (std::size_t p = 0; p < N; ++p)
                        {
                            const Real *state = pilot.state_row(p);
                            const Real exercise = tails_[k]->value(i, state);
                            if (exercises(i, exercise, state[i]))
                                held[p] = exercise * pilot.deflator_row(p)[i];
                        }
                    }
                }
                fitted_ = true;
            }

            Real value(std::size_t i, const Real *state) const override
            {
                const std::size_t k = exercised_by(i, state);
                if (k != kNever)
                    return tails_[k]->value(i, state);
                // An option not exercised is worth at least nothing; the
                // polynomial, far out of the money, can say otherwise.
                return std::max(continuation_[i](state + i), 0.0);
            }

            Real cashflow(std::size_t i, const Real *state) const override
            {
                const std::size_t k = exercised_by(i, state);
                // The swap's first coupon pays after the exercise date.
                return k != kNever && exercise_index_[k] < i ? tails_[k]->cashflow(i, state) : 0.0;
            }

            /// 0 while it is an option; 1 + k once exercised on date k.
            std::size_t regime(std::size_t i, const Real *state) const override
            {
                const std::size_t k = exercised_by(i, state);
                return k == kNever ? 0 : 1 + k;
            }

            std::size_t regressors(std::size_t i, const Real *state, Real *out) const override
            {
                const std::size_t k = exercised_by(i, state);
                if (k != kNever)
                    return tails_[k]->regressors(i, state, out);
                out[0] = state[i];
                return 1;
            }

          private:
            /// The holder's rule: enter the swap when it is worth something,
            /// and more than keeping the option.
            bool exercises(std::size_t i, Real exercise, Real x) const
            {
                return exercise > 0.0 && exercise >= continuation_[i](&x);
            }

            /// The exercise date (its index among them) on which the path
            /// exercised, at or before grid[i]; kNever if it has not.
            std::size_t exercised_by(std::size_t i, const Real *state) const
            {
                if (!fitted_)
                    throw InvalidInput("Bermudan future value: fit() must run before the simulation");
                for (std::size_t k = 0; k < tails_.size() && exercise_index_[k] <= i; ++k)
                {
                    const std::size_t e = exercise_index_[k];
                    if (exercises(e, tails_[k]->value(e, state), state[e]))
                        return k;
                }
                return kNever;
            }

            Real today_;
            std::vector<Time> exercise_times_;
            AmcSettings settings_;
            Time maturity_;
            /// tails_[k]: the swap entered by exercising on date k.
            std::vector<std::unique_ptr<FutureValue>> tails_;
            std::vector<std::size_t> exercise_index_;
            /// Per grid date: the value of the option not yet exercised, as
            /// a function of the state (empty after the last exercise date).
            std::vector<BucketedRegression> continuation_;
            bool fitted_ = false;
        };
    } // namespace

    std::unique_ptr<FutureValue> make_future_value(const BermudanSwaption &bermudan,
                                                   const HullWhiteCurveModel &model,
                                                   const AmcSettings &settings)
    {
        return std::make_unique<HullWhiteBermudanValue>(bermudan, model, settings);
    }

} // namespace quantModeling
