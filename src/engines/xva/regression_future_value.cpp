#include "quantModeling/engines/xva/regression_future_value.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace quantModeling
{
    namespace
    {
        class RegressionFutureValue final : public FutureValue
        {
          public:
            RegressionFutureValue(std::unique_ptr<FutureValue> trade, const AmcSettings &settings)
                : trade_(std::move(trade)), settings_(settings)
            {
                if (!trade_)
                    throw InvalidInput("AMC: null trade");
                resolved(settings_); // validates
            }

            std::vector<Time> event_times() const override { return trade_->event_times(); }
            Time maturity() const override { return trade_->maturity(); }
            Real value_today() const override { return trade_->value_today(); }

            void bind(const std::vector<Time> &grid) override
            {
                trade_->bind(grid);
                fits_.assign(grid.size(), {});
            }

            bool needs_pilot() const override { return true; }

            void fit(const PilotPaths &pilot) override
            {
                // The trade itself may be valued by regression (its regime
                // may depend on a fitted exercise rule).
                if (trade_->needs_pilot())
                    trade_->fit(pilot);
                const std::size_t n = pilot.dates();
                const std::size_t N = pilot.paths;
                if (n != fits_.size())
                    throw InvalidInput("AMC: the pilot paths are not on the trade's grid");

                // future[p] = Σ_{j>i} flow_j deflator_j on path p, built
                // backward: what the value at t_i is the expectation of, once
                // divided by deflator_i.
                std::vector<Real> future(N, 0.0);
                struct Sample
                {
                    std::size_t regressors = 0;
                    std::vector<Real> rows;
                    std::vector<Real> targets;
                };
                std::array<Real, kMaxRegressors> z{};
                for (std::size_t i = n; i-- > 0;)
                {
                    std::map<std::size_t, Sample> by_regime;
                    for (std::size_t p = 0; p < N; ++p)
                    {
                        const Real *state = pilot.state_row(p);
                        Sample &sample = by_regime[trade_->regime(i, state)];
                        const std::size_t count = trade_->regressors(i, state, z.data());
                        if (sample.targets.empty())
                            sample.regressors = count;
                        else if (count != sample.regressors)
                            throw InvalidInput("AMC: a regime must have the same regressors on "
                                               "every path at a given date");
                        sample.rows.insert(sample.rows.end(), z.begin(),
                                           z.begin() + static_cast<std::ptrdiff_t>(count));
                        sample.targets.push_back(future[p] / pilot.deflator_row(p)[i]);
                    }
                    for (auto &[regime, sample] : by_regime)
                        fits_[i].emplace_back(regime,
                                              BucketedRegression::fit(sample.rows, sample.regressors,
                                                                      sample.targets, settings_));
                    for (std::size_t p = 0; p < N; ++p)
                        future[p] += trade_->cashflow(i, pilot.state_row(p)) * pilot.deflator_row(p)[i];
                }
            }

            Real value(std::size_t i, const Real *state) const override
            {
                const std::size_t regime = trade_->regime(i, state);
                for (const auto &[r, regression] : fits_[i])
                    if (r == regime)
                    {
                        std::array<Real, kMaxRegressors> z{};
                        trade_->regressors(i, state, z.data());
                        return regression(z.data());
                    }
                // A regime no pilot path was in: nothing to estimate it from.
                return 0.0;
            }

            Real cashflow(std::size_t i, const Real *state) const override
            {
                return trade_->cashflow(i, state);
            }

            std::size_t regime(std::size_t i, const Real *state) const override
            {
                return trade_->regime(i, state);
            }

            std::size_t regressors(std::size_t i, const Real *state, Real *out) const override
            {
                return trade_->regressors(i, state, out);
            }

          private:
            std::unique_ptr<FutureValue> trade_;
            AmcSettings settings_;
            /// Per date: one regression per regime seen on the pilot paths.
            std::vector<std::vector<std::pair<std::size_t, BucketedRegression>>> fits_;
        };
    } // namespace

    std::unique_ptr<FutureValue> make_regression_future_value(std::unique_ptr<FutureValue> trade,
                                                              const AmcSettings &settings)
    {
        return std::make_unique<RegressionFutureValue>(std::move(trade), settings);
    }

} // namespace quantModeling
