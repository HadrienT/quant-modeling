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
        /// Points per basis function below which a degree is not trusted.
        constexpr std::size_t kRowsPerTerm = 10;
        constexpr int kMaxDegree = 6;
        constexpr int kMaxBuckets = 64;
    } // namespace

    AmcSettings resolved(const AmcSettings &settings)
    {
        if (settings.degree < 0 || settings.degree > kMaxDegree)
            throw InvalidInput("AMC: the degree of the basis must be between 0 (automatic) and " +
                               std::to_string(kMaxDegree));
        if (settings.buckets < 0 || settings.buckets > kMaxBuckets)
            throw InvalidInput("AMC: the number of buckets must be between 0 (automatic) and " +
                               std::to_string(kMaxBuckets));
        return {settings.degree > 0 ? settings.degree : 2, settings.buckets > 0 ? settings.buckets : 4};
    }

    PolynomialRegression PolynomialRegression::fit(const std::vector<Real> &rows, std::size_t n,
                                                   const std::vector<Real> &targets, int degree)
    {
        PolynomialRegression out;
        const std::size_t count = targets.size();
        if (count == 0)
            return out;
        if (n == 0 || n > FutureValue::kMaxRegressors || rows.size() != count * n)
            throw InvalidInput("AMC regression: inconsistent regressors");
        // Fewer points than the basis can be trusted with: a lower degree,
        // down to the mean of the targets.
        while (degree > 0 && count < kRowsPerTerm * scripting::monomials(n, degree).size())
            --degree;
        std::vector<std::vector<double>> table(count, std::vector<double>(n));
        for (std::size_t r = 0; r < count; ++r)
            std::copy_n(rows.begin() + static_cast<std::ptrdiff_t>(r * n), n, table[r].begin());
        out.fit_ = scripting::fit_exercise_regression(table, targets, degree);
        return out;
    }

    Real PolynomialRegression::operator()(const Real *regressors) const
    {
        if (fit_.beta.empty())
            return 0.0;
        // Powers of each standardised regressor, then one product per monomial.
        std::array<std::array<Real, kMaxDegree + 1>, FutureValue::kMaxRegressors> power;
        const std::size_t n = fit_.mean.size();
        for (std::size_t k = 0; k < n; ++k)
        {
            const Real z = (regressors[k] - fit_.mean[k]) / fit_.scale[k];
            power[k][0] = 1.0;
            for (int d = 1; d <= fit_.degree; ++d)
                power[k][static_cast<std::size_t>(d)] = power[k][static_cast<std::size_t>(d - 1)] * z;
        }
        Real value = 0.0;
        for (std::size_t t = 0; t < fit_.terms.size(); ++t)
        {
            Real phi = 1.0;
            for (std::size_t k = 0; k < n; ++k)
                phi *= power[k][static_cast<std::size_t>(fit_.terms[t][k])];
            value += fit_.beta[t] * phi;
        }
        return value;
    }

    BucketedRegression BucketedRegression::fit(const std::vector<Real> &rows, std::size_t n,
                                               const std::vector<Real> &targets,
                                               const AmcSettings &settings)
    {
        const AmcSettings s = resolved(settings);
        BucketedRegression out;
        const std::size_t count = targets.size();
        if (count == 0)
            return out;
        if (n == 0 || n > FutureValue::kMaxRegressors || rows.size() != count * n)
            throw InvalidInput("AMC regression: inconsistent regressors");

        // As many buckets as asked, unless they would be too thin for the
        // polynomial each one carries.
        const std::size_t terms = scripting::monomials(n, s.degree).size();
        const std::size_t buckets = std::clamp<std::size_t>(count / (kRowsPerTerm * terms), 1,
                                                            static_cast<std::size_t>(s.buckets));
        std::vector<std::size_t> order(count);
        for (std::size_t r = 0; r < count; ++r)
            order[r] = r;
        // Ties keep the order of the paths: the fit does not depend on the
        // sorting algorithm.
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
                         { return rows[a * n] < rows[b * n]; });
        std::vector<Real> bucket_rows, bucket_targets;
        for (std::size_t b = 0; b < buckets; ++b)
        {
            const std::size_t first = b * count / buckets, last = (b + 1) * count / buckets;
            bucket_rows.clear();
            bucket_targets.clear();
            for (std::size_t r = first; r < last; ++r)
            {
                const std::size_t row = order[r];
                bucket_rows.insert(bucket_rows.end(),
                                   rows.begin() + static_cast<std::ptrdiff_t>(row * n),
                                   rows.begin() + static_cast<std::ptrdiff_t>((row + 1) * n));
                bucket_targets.push_back(targets[row]);
            }
            out.buckets_.push_back(PolynomialRegression::fit(bucket_rows, n, bucket_targets, s.degree));
            if (b + 1 < buckets)
                out.edges_.push_back(0.5 * (rows[order[last - 1] * n] + rows[order[last] * n]));
        }
        return out;
    }

    Real BucketedRegression::operator()(const Real *regressors) const
    {
        if (buckets_.empty())
            return 0.0;
        const std::size_t b = static_cast<std::size_t>(
            std::upper_bound(edges_.begin(), edges_.end(), regressors[0]) - edges_.begin());
        return buckets_[b](regressors);
    }

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
