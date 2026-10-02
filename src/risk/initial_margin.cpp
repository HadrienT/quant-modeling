#include "quantModeling/risk/initial_margin.hpp"

#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/utils/inverse_normal.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace quantModeling
{
    namespace
    {
        std::vector<std::size_t> netted(const ExposurePaths &paths, std::vector<std::size_t> trades)
        {
            if (paths.dates() == 0 || paths.paths == 0 || paths.trades() == 0)
                throw InvalidInput("initial margin: the simulation is empty");
            if (trades.empty())
            {
                trades.resize(paths.trades());
                std::iota(trades.begin(), trades.end(), std::size_t{0});
            }
            for (const std::size_t k : trades)
                if (k >= paths.trades())
                    throw InvalidInput("initial margin: trade index out of range");
            return trades;
        }

        Real value_today(const ExposurePaths &paths, const std::vector<std::size_t> &trades)
        {
            Real v = 0.0;
            for (const std::size_t k : trades)
                v += paths.trade_values_today[k];
            return v;
        }

        /// Value of the netting set at (path, date).
        Real value_at(const ExposurePaths &paths, const std::vector<std::size_t> &trades,
                      std::size_t p, std::size_t i)
        {
            Real v = 0.0;
            for (const std::size_t k : trades)
                v += paths.trade_values[k][p * paths.dates() + i];
            return v;
        }

        /**
         * The move of the netting set over the margin period ending at date
         * `pair.date`: V(t) - V(t - MPoR), plus the flows paid in between
         * when the cube kept them — a coupon the netting set received is not
         * a loss of value.
         */
        Real move(const ExposurePaths &paths, const std::vector<std::size_t> &trades,
                  const MarginPeriod &pair, Real today, std::size_t p)
        {
            const std::size_t n = paths.dates();
            Real m = value_at(paths, trades, p, pair.date) -
                     (pair.lagged == MarginPeriod::kToday ? today
                                                          : value_at(paths, trades, p, pair.lagged));
            if (paths.trade_cashflows.size() == paths.trades())
            {
                const std::size_t first = pair.lagged == MarginPeriod::kToday ? 0 : pair.lagged + 1;
                for (const std::size_t k : trades)
                    for (std::size_t j = first; j <= pair.date; ++j)
                        m += paths.trade_cashflows[k][p * n + j];
            }
            return m;
        }

        std::vector<MarginPeriod> periods(const ExposurePaths &paths, Time mpor)
        {
            if (!(mpor > 0.0) || !std::isfinite(mpor))
                throw InvalidInput("initial margin: the margin period of risk must be > 0");
            const std::vector<MarginPeriod> pairs = margin_periods(paths.times, mpor);
            if (pairs.empty() || pairs.back().date != paths.dates() - 1)
                throw InvalidInput(
                    "initial margin: the last date of the simulation has no lagged date t - MPoR "
                    "on the grid; simulate with grid.margin_period_of_risk set to it");
            return pairs;
        }
    } // namespace

    DynamicInitialMargin DynamicInitialMargin::fit(const ExposurePaths &paths, Time mpor,
                                                   const std::vector<std::size_t> &trades,
                                                   const DimSettings &settings)
    {
        if (!(settings.confidence > 0.5 && settings.confidence < 1.0))
            throw InvalidInput("initial margin: the confidence level must be in (0.5, 1)");
        if (settings.im_today && !(*settings.im_today >= 0.0))
            throw InvalidInput("initial margin: today's margin must be >= 0");

        DynamicInitialMargin model;
        model.trades_ = netted(paths, trades);
        model.grid_ = paths.times;
        model.mpor_ = mpor;
        model.quantile_ = inverse_normal_cdf(settings.confidence);
        const std::vector<MarginPeriod> pairs = periods(paths, mpor);
        const std::size_t N = paths.paths;
        const Real today = value_today(paths, model.trades_);

        std::vector<Real> rows(N), targets(N);
        const MarginPeriod *longest_from_today = nullptr;
        for (const MarginPeriod &pair : pairs)
        {
            Date date{pair.date, pair.lagged, {}};
            if (pair.lagged == MarginPeriod::kToday)
            {
                // Periods that start today: one state, nothing to regress.
                // The longest of them is the closest to a full MPoR.
                longest_from_today = &pair;
            }
            else
            {
                for (std::size_t p = 0; p < N; ++p)
                {
                    const Real m = move(paths, model.trades_, pair, today, p);
                    rows[p] = value_at(paths, model.trades_, p, pair.lagged);
                    // The drift over ten days is negligible next to the move.
                    targets[p] = m * m;
                }
                date.variance = BucketedRegression::fit(rows, 1, targets, settings.regression);
            }
            model.dates_.push_back(std::move(date));
        }

        // Today's margin: the standard deviation of the move over the longest
        // period that starts today, scaled to a full MPoR by the square root
        // of time.
        if (longest_from_today != nullptr)
        {
            Real squares = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real m = move(paths, model.trades_, *longest_from_today, today, p);
                squares += m * m;
            }
            const Time length = paths.times[longest_from_today->date];
            model.today_ =
                model.quantile_ * std::sqrt(squares / static_cast<Real>(N) * mpor / length);
        }
        if (settings.im_today)
        {
            if (!(model.today_ > 0.0))
                throw InvalidInput("initial margin: the model's margin today is zero, it cannot be "
                                   "scaled to the given amount");
            model.scaling_ = *settings.im_today / model.today_;
        }
        return model;
    }

    DynamicInitialMargin DynamicInitialMargin::scaled(Real scaling) const
    {
        if (!(scaling >= 0.0) || !std::isfinite(scaling))
            throw InvalidInput("initial margin: the scaling must be finite and >= 0");
        DynamicInitialMargin copy = *this;
        copy.scaling_ = scaling;
        return copy;
    }

    InitialMargin DynamicInitialMargin::margin(const ExposurePaths &paths) const
    {
        if (paths.times != grid_)
            throw InvalidInput("initial margin: these paths are not on the grid the model was "
                               "fitted on");
        for (const std::size_t k : trades_)
            if (k >= paths.trades())
                throw InvalidInput("initial margin: trade index out of range");
        const std::size_t n = paths.dates(), N = paths.paths, m = dates_.size();

        InitialMargin out;
        out.paths = N;
        out.today = today();
        out.scaling = scaling_;
        out.margin.resize(N * m);
        out.expected.assign(m, 0.0);
        out.discounted_expected.assign(m, 0.0);
        for (std::size_t r = 0; r < m; ++r)
        {
            const Date &date = dates_[r];
            out.times.push_back(paths.times[date.index]);
            Real sum = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                Real im = out.today;
                if (date.lagged != MarginPeriod::kToday)
                {
                    const Real v = value_at(paths, trades_, p, date.lagged);
                    // A fitted variance can dip below zero at the edges.
                    im = scaling_ * quantile_ * std::sqrt(std::max(date.variance(&v), 0.0));
                }
                out.margin[p * m + r] = im;
                sum += paths.discount_weight[p * n + date.index] * im;
            }
            out.discounted_expected[r] = sum / static_cast<Real>(N);
            out.expected[r] = out.discounted_expected[r] / paths.discount[date.index];
        }
        return out;
    }

    InitialMargin simm_initial_margin(const ExposurePaths &paths, Time mpor)
    {
        const std::size_t n = paths.dates(), N = paths.paths;
        if (paths.simm.size() != N * n || N == 0)
            throw InvalidInput("initial margin: this simulation did not compute SIMM; simulate "
                               "with settings.simm = true");
        const std::vector<MarginPeriod> pairs = periods(paths, mpor);
        const std::size_t m = pairs.size();
        InitialMargin out;
        out.paths = N;
        out.today = paths.simm_today.total();
        out.margin.resize(N * m);
        out.expected.assign(m, 0.0);
        out.discounted_expected.assign(m, 0.0);
        for (std::size_t r = 0; r < m; ++r)
        {
            const MarginPeriod &pair = pairs[r];
            out.times.push_back(paths.times[pair.date]);
            Real sum = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real im = pair.lagged == MarginPeriod::kToday
                                    ? out.today
                                    : paths.simm[p * n + pair.lagged];
                out.margin[p * m + r] = im;
                sum += paths.discount_weight[p * n + pair.date] * im;
            }
            out.discounted_expected[r] = sum / static_cast<Real>(N);
            out.expected[r] = out.discounted_expected[r] / paths.discount[pair.date];
        }
        return out;
    }

    MarginBacktest backtest_initial_margin(const ExposurePaths &paths, const InitialMargin &margin,
                                           Time mpor, const std::vector<std::size_t> &trades)
    {
        const std::vector<std::size_t> set = netted(paths, trades);
        const std::vector<MarginPeriod> pairs = periods(paths, mpor);
        const std::size_t N = paths.paths, m = pairs.size();
        if (margin.paths != N || margin.times.size() != m || margin.margin.size() != N * m)
            throw InvalidInput("initial margin backtest: the margin is not that of these paths");
        const Real today = value_today(paths, set);

        MarginBacktest out;
        for (std::size_t r = 0; r < m; ++r)
        {
            std::size_t up = 0, down = 0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real mv = move(paths, set, pairs[r], today, p);
                up += mv > margin.margin[p * m + r];
                down += -mv > margin.margin[p * m + r];
            }
            out.times.push_back(paths.times[pairs[r].date]);
            out.uncovered_bank.push_back(static_cast<Real>(up) / static_cast<Real>(N));
            out.uncovered_counterparty.push_back(static_cast<Real>(down) / static_cast<Real>(N));
            out.full_period.push_back(pairs[r].lagged != MarginPeriod::kToday ||
                                      paths.times[pairs[r].date] > mpor - 1e-10);
        }
        return out;
    }

} // namespace quantModeling
