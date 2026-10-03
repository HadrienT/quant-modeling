#include "quantModeling/risk/xva_report.hpp"

#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/wrong_way_risk.hpp"
#include "quantModeling/risk/xva.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace quantModeling
{
    namespace
    {
        /// The initial margin of a set of trades on these paths, with the
        /// netting set's scaling.
        InitialMargin initial_margin(const ExposurePaths &paths, const XvaInputs &in,
                                     const std::vector<std::size_t> &trades, Real scaling)
        {
            DimSettings settings = *in.initial_margin;
            settings.im_today.reset();
            return DynamicInitialMargin::fit(paths, in.csa->margin_period_of_risk, trades, settings)
                .scaled(scaling)
                .margin(paths);
        }

        /// The cube of one netting set: collateralised when there is a CSA
        /// (with the initial margin on both sides when `margin` is given),
        /// the given trades otherwise.
        ExposurePaths netting_set_cube(const ExposurePaths &paths, const XvaInputs &in,
                                       const std::vector<std::size_t> &trades,
                                       const InitialMargin *margin = nullptr)
        {
            if (in.csa)
            {
                if (margin == nullptr)
                    return collateralise(paths, *in.csa, trades, in.collateral);
                CollateralSettings settings = in.collateral;
                settings.initial_margin_received_paths = margin->margin;
                settings.initial_margin_posted_paths = margin->margin;
                return collateralise(paths, *in.csa, trades, settings);
            }
            ExposurePaths out;
            out.measure = paths.measure;
            out.times = paths.times;
            out.discount = paths.discount;
            out.paths = paths.paths;
            out.discount_weight = paths.discount_weight;
            out.trade_values.assign(1, std::vector<Real>(paths.paths * paths.dates(), 0.0));
            Real today = 0.0;
            for (const std::size_t k : trades)
            {
                today += paths.trade_values_today[k];
                const std::vector<Real> &values = paths.trade_values[k];
                for (std::size_t j = 0; j < values.size(); ++j)
                    out.trade_values[0][j] += values[j];
            }
            out.trade_values_today = {today};
            return out;
        }

        /// CVA and DVA of a single-trade cube, path by path: the mean of the
        /// per-path losses is the integral of the mean exposure, and their
        /// dispersion gives the Monte-Carlo error, correlations between
        /// dates included.
        void pathwise_adjustments(const ExposurePaths &cube, const XvaInputs &in, Estimate &cva,
                                  Estimate &dva)
        {
            const std::size_t n = cube.dates(), N = cube.paths;
            const std::vector<Real> pd_counterparty = first_to_default_weights(cube.times, in.counterparty, in.own);
            const std::vector<Real> pd_own = first_to_default_weights(cube.times, in.own, in.counterparty);
            Real c = 0.0, c2 = 0.0, d = 0.0, d2 = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                Real loss = 0.0, gain = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Real v = cube.discount_weight[p * n + i] * cube.trade_values[0][p * n + i];
                    loss += std::max(v, 0.0) * pd_counterparty[i];
                    gain += std::min(v, 0.0) * pd_own[i];
                }
                loss *= -in.lgd_counterparty;
                gain *= -in.lgd_own;
                c += loss;
                c2 += loss * loss;
                d += gain;
                d2 += gain * gain;
            }
            const Real count = static_cast<Real>(N);
            const auto estimate = [count, N](Real sum, Real sum2)
            {
                Estimate e;
                e.value = sum / count;
                if (N > 1)
                    e.error = std::sqrt(std::max(sum2 / count - e.value * e.value, 0.0) / (count - 1.0));
                return e;
            };
            cva = estimate(c, c2);
            dva = estimate(d, d2);
        }

        /**
         * The Monte-Carlo error of running-cost adjustments
         *
         *   -rate Σ_i E[X*(t_i)] S_C(t_i) S_I(t_i) Δt_i
         *
         * (risk/xva.hpp, running_cost_adjustment) from the quantity on each
         * path: `discounted(p, i, out)` writes the discounted X of path p at
         * times[i] for each of the K adjustments, already times its rate.
         * The last output is the error of their sum.
         */
        template <std::size_t K, class F>
        std::array<Real, K + 1> running_cost_errors(const std::vector<Time> &times, std::size_t N,
                                                    const XvaInputs &in, F discounted)
        {
            const std::size_t n = times.size();
            std::vector<Real> weight(n);
            Time previous = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                weight[i] = in.counterparty.survival(times[i]) * in.own.survival(times[i]) *
                            (times[i] - previous);
                previous = times[i];
            }
            std::array<Real, K + 1> sum{}, sum2{}, errors{};
            std::array<Real, K> x{};
            for (std::size_t p = 0; p < N; ++p)
            {
                std::array<Real, K + 1> path{};
                for (std::size_t i = 0; i < n; ++i)
                {
                    discounted(p, i, x);
                    for (std::size_t k = 0; k < K; ++k)
                        path[k] -= x[k] * weight[i];
                }
                for (std::size_t k = 0; k < K; ++k)
                    path[K] += path[k];
                for (std::size_t k = 0; k <= K; ++k)
                {
                    sum[k] += path[k];
                    sum2[k] += path[k] * path[k];
                }
            }
            if (N < 2)
                return errors;
            const Real count = static_cast<Real>(N);
            for (std::size_t k = 0; k <= K; ++k)
            {
                const Real mean = sum[k] / count;
                errors[k] = std::sqrt(std::max(sum2[k] / count - mean * mean, 0.0) / (count - 1.0));
            }
            return errors;
        }

        /// The paths [first, first + count) of a one-trade cube, and of the
        /// margin that goes with it.
        ExposurePaths batch_of(const ExposurePaths &cube, std::size_t first, std::size_t count)
        {
            const std::size_t n = cube.dates();
            const auto rows = [first, count, n](const std::vector<Real> &all)
            {
                return std::vector<Real>(all.begin() + static_cast<std::ptrdiff_t>(first * n),
                                         all.begin() + static_cast<std::ptrdiff_t>((first + count) * n));
            };
            ExposurePaths out;
            out.measure = cube.measure;
            out.times = cube.times;
            out.discount = cube.discount;
            out.paths = count;
            out.discount_weight = rows(cube.discount_weight);
            out.trade_values = {rows(cube.trade_values[0])};
            out.trade_values_today = cube.trade_values_today;
            return out;
        }

        InitialMargin batch_of(const InitialMargin &margin, std::size_t first, std::size_t count)
        {
            const std::size_t n = margin.times.size();
            InitialMargin out;
            out.times = margin.times;
            out.paths = count;
            out.today = margin.today;
            out.scaling = margin.scaling;
            out.margin.assign(margin.margin.begin() + static_cast<std::ptrdiff_t>(first * n),
                              margin.margin.begin() + static_cast<std::ptrdiff_t>((first + count) * n));
            return out;
        }

        /// The survival of every path on the dates of `cube` (the reporting
        /// dates of a collateralised cube are some of the grid's).
        PathwiseSurvival on_dates_of(const PathwiseSurvival &full, const ExposurePaths &cube)
        {
            if (full.times == cube.times)
                return full;
            PathwiseSurvival out;
            out.times = cube.times;
            out.paths = full.paths;
            out.b = full.b;
            const std::size_t n = full.dates(), m = cube.dates();
            out.survival.resize(full.paths * m);
            std::size_t i = 0;
            for (std::size_t r = 0; r < m; ++r)
            {
                while (i < n && full.times[i] < cube.times[r] - 1e-10)
                    ++i;
                if (i == n || std::abs(full.times[i] - cube.times[r]) > 1e-10)
                    throw InvalidInput("xVA report: a reporting date is not on the simulation grid");
                for (std::size_t p = 0; p < full.paths; ++p)
                    out.survival[p * m + r] = full.survival[p * n + i];
            }
            return out;
        }

        /**
         * CVA and DVA when the counterparty's survival is the path's own
         * (wrong-way risk). Per path,
         *
         *   loss = -LGD_C Σ_i D max(V, 0) S_I(t_{i-1}) (q_{i-1} - q_i)
         *   gain = -LGD_I Σ_i D min(V, 0) q_{i-1} (S_I(t_{i-1}) - S_I(t_i))
         *
         * which are those of pathwise_adjustments when q is the market curve.
         */
        void wrong_way_adjustments(const ExposurePaths &cube, const XvaInputs &in,
                                   const PathwiseSurvival &q, Estimate &cva, Estimate &dva)
        {
            const std::size_t n = cube.dates(), N = cube.paths;
            std::vector<Real> own_before(n), own_default(n);
            Time previous = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                own_before[i] = in.own.survival(previous);
                own_default[i] = own_before[i] - in.own.survival(cube.times[i]);
                previous = cube.times[i];
            }
            Real c = 0.0, c2 = 0.0, d = 0.0, d2 = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                Real loss = 0.0, gain = 0.0;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Real v = cube.discount_weight[p * n + i] * cube.trade_values[0][p * n + i];
                    const Real before = q.before(p, i);
                    loss += std::max(v, 0.0) * own_before[i] * (before - q.survival[p * n + i]);
                    gain += std::min(v, 0.0) * before * own_default[i];
                }
                loss *= -in.lgd_counterparty;
                gain *= -in.lgd_own;
                c += loss;
                c2 += loss * loss;
                d += gain;
                d2 += gain * gain;
            }
            const Real count = static_cast<Real>(N);
            const auto estimate = [count, N](Real sum, Real sum2)
            {
                Estimate e;
                e.value = sum / count;
                if (N > 1)
                    e.error = std::sqrt(std::max(sum2 / count - e.value * e.value, 0.0) / (count - 1.0));
                return e;
            };
            cva = estimate(c, c2);
            dva = estimate(d, d2);
        }

        /// Bilateral CVA of a subset of the trades, under the same CSA and,
        /// when there is one, its own initial margin; under wrong-way risk,
        /// with the hazard driven by the subset's own value.
        Real bilateral_cva(const ExposurePaths &paths, const XvaInputs &in,
                           const std::vector<std::size_t> &trades, Real margin_scaling)
        {
            ExposurePaths cube;
            if (in.initial_margin)
            {
                const InitialMargin margin = initial_margin(paths, in, trades, margin_scaling);
                cube = netting_set_cube(paths, in, trades, &margin);
            }
            else
                cube = netting_set_cube(paths, in, trades);
            if (in.wrong_way_b != 0.0)
            {
                Estimate cva, dva;
                wrong_way_adjustments(
                    cube, in,
                    on_dates_of(wrong_way_survival(paths, trades, in.counterparty, in.wrong_way_b),
                                cube),
                    cva, dva);
                return cva.value;
            }
            return cva_bilateral(exposure_profile(cube), in.counterparty, in.own,
                                 in.lgd_counterparty);
        }
    } // namespace

    std::vector<Real> first_to_default_weights(const std::vector<Time> &times,
                                               const CreditCurve &defaulter, const CreditCurve &other)
    {
        std::vector<Real> weights(times.size());
        Time previous = 0.0;
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            weights[i] = other.survival(previous) *
                         (defaulter.survival(previous) - defaulter.survival(times[i]));
            previous = times[i];
        }
        return weights;
    }

    XvaReport xva_report(const ExposurePaths &paths, const XvaInputs &in)
    {
        if (paths.measure != ExposureMeasure::RiskNeutral)
            throw InvalidInput("xVA report: adjustments are prices and need a risk-neutral "
                               "simulation; this one ran under the historical measure");
        std::vector<std::size_t> trades = in.trades;
        if (trades.empty())
        {
            trades.resize(paths.trades());
            std::iota(trades.begin(), trades.end(), std::size_t{0});
        }
        std::vector<bool> seen(paths.trades(), false);
        for (const std::size_t k : trades)
        {
            if (k >= paths.trades())
                throw InvalidInput("xVA report: trade index out of range");
            if (seen[k])
                throw InvalidInput("xVA report: a trade is netted twice");
            seen[k] = true;
        }
        if (!(in.lgd_counterparty >= 0.0 && in.lgd_counterparty <= 1.0) ||
            !(in.lgd_own >= 0.0 && in.lgd_own <= 1.0))
            throw InvalidInput("xVA report: loss given default must be in [0, 1]");

        if (in.initial_margin && !in.csa)
            throw InvalidInput("xVA report: initial margin needs a collateral agreement");
        if (in.capital && in.capital->method == ExposureMethod::SaCcr &&
            in.capital->trades.size() != trades.size())
            throw InvalidInput("xVA report: the capital inputs must describe each netted trade");
        if (!std::isfinite(in.initial_margin_spread) || !std::isfinite(in.collateral_spread) ||
            !(in.cost_of_capital >= 0.0))
            throw InvalidInput("xVA report: spreads must be finite and the cost of capital >= 0");

        XvaReport report;
        // Without initial margin: what variation margin alone leaves, the
        // funding requirement, and the collateral that capital recognises.
        const ExposurePaths variation_only = netting_set_cube(paths, in, trades);
        std::optional<InitialMargin> margin;
        Real margin_scaling = 1.0;
        if (in.initial_margin)
        {
            DimSettings settings = *in.initial_margin;
            const bool simm = in.margin_model != XvaInputs::MarginModel::Regression;
            if (simm)
            {
                if (paths.simm.empty())
                    throw InvalidInput("xVA report: a SIMM margin needs a simulation that computed "
                                       "SIMM (settings.simm = true)");
                if (trades.size() != paths.trades())
                    throw InvalidInput("xVA report: the simulation's SIMM is that of all its "
                                       "trades, not of a subset");
                // Today's margin is the rule's, not the model's.
                settings.im_today = paths.simm_today.total();
            }
            const DynamicInitialMargin model =
                DynamicInitialMargin::fit(paths, in.csa->margin_period_of_risk, trades, settings);
            // The factor the shares of each trade are computed with.
            margin_scaling = model.scaling();
            margin = in.margin_model == XvaInputs::MarginModel::SimmPerPath
                         ? simm_initial_margin(paths, in.csa->margin_period_of_risk)
                         : model.margin(paths);
            report.initial_margin_today = margin->today;
            report.expected_initial_margin = margin->expected;
            report.mva = mva(margin->times, margin->discounted_expected, in.counterparty, in.own,
                             in.initial_margin_spread);
        }
        const std::size_t N = paths.paths;
        const ExposurePaths cube =
            margin ? netting_set_cube(paths, in, trades, &*margin) : variation_only;
        report.exposure = exposure_statistics(cube, {}, in.pfe_confidence, in.quantile_levels);
        const ExposureProfile profile = report.exposure.profile();

        pathwise_adjustments(cube, in, report.cva_independent, report.dva_independent);
        if (in.wrong_way_b != 0.0)
            wrong_way_adjustments(
                cube, in,
                on_dates_of(wrong_way_survival(paths, trades, in.counterparty, in.wrong_way_b), cube),
                report.cva, report.dva);
        else
        {
            report.cva = report.cva_independent;
            report.dva = report.dva_independent;
        }
        report.cva_unilateral = cva_unilateral(profile, in.counterparty, in.lgd_counterparty);
        // Segregated initial margin funds nothing: FVA is on the cube
        // without it.
        const ExposureProfile funding =
            margin ? exposure_profile(variation_only) : profile;
        report.fca = fca(funding, in.counterparty, in.own, in.borrowing_spread);
        report.fba = fba(funding, in.counterparty, in.own, in.lending_spread);
        {
            const ExposurePaths &funded = margin ? variation_only : cube;
            const std::size_t n = funded.dates();
            const auto errors = running_cost_errors<2>(
                funded.times, N, in,
                [&funded, &in, n](std::size_t p, std::size_t i, std::array<Real, 2> &x)
                {
                    const Real v = funded.discount_weight[p * n + i] * funded.trade_values[0][p * n + i];
                    x[0] = in.borrowing_spread * std::max(v, 0.0);
                    x[1] = in.lending_spread * std::min(v, 0.0);
                });
            report.fca_error = errors[0];
            report.fba_error = errors[1];
            report.fva_error = errors[2];
        }
        if (margin)
        {
            // The margin is on the dates of the collateralised cube.
            const std::size_t n = cube.dates();
            report.mva_error = running_cost_errors<1>(
                cube.times, N, in,
                [&cube, &margin, &in, n](std::size_t p, std::size_t i, std::array<Real, 1> &x)
                {
                    x[0] = in.initial_margin_spread * cube.discount_weight[p * n + i] *
                           margin->margin[p * n + i];
                })[0];
        }
        if (in.csa)
        {
            const std::vector<Real> collateral = discounted_collateral_paths(paths, *in.csa, trades);
            const std::size_t m = variation_only.dates();
            std::vector<Real> expected(m, 0.0);
            for (std::size_t r = 0; r < m; ++r)
            {
                Real sum = 0.0;
                for (std::size_t p = 0; p < N; ++p)
                    sum += collateral[p * m + r];
                expected[r] = sum / static_cast<Real>(N);
            }
            report.colva = colva(variation_only.times, expected, in.counterparty, in.own,
                                 in.collateral_spread);
            report.colva_error = running_cost_errors<1>(
                variation_only.times, N, in,
                [&collateral, &in, m](std::size_t p, std::size_t r, std::array<Real, 1> &x)
                { x[0] = in.collateral_spread * collateral[p * m + r]; })[0];
        }
        if (in.capital)
        {
            report.capital =
                projected_capital(variation_only, *in.capital, margin ? &*margin : nullptr);
            report.kva = kva(report.capital->times, report.capital->discounted_capital,
                             in.counterparty, in.own, in.cost_of_capital);
            // The same projection on each batch of paths.
            const std::size_t B = error_batches(N);
            std::vector<Real> batches(B);
            for (std::size_t b = 0; b < B; ++b)
            {
                const std::size_t size = N / B, first = b * size;
                const std::size_t count = b + 1 < B ? size : N - first;
                const ExposurePaths part = batch_of(variation_only, first, count);
                std::optional<InitialMargin> held;
                if (margin)
                    held = batch_of(*margin, first, count);
                const CapitalProfile capital =
                    projected_capital(part, *in.capital, held ? &*held : nullptr);
                batches[b] = kva(capital.times, capital.discounted_capital, in.counterparty, in.own,
                                 in.cost_of_capital);
            }
            report.kva_error = batch_error(batches);
        }

        const Time maturity = profile.times.back();
        const Real average_hazard = -std::log(in.counterparty.survival(maturity)) / maturity;
        report.cva_rule_of_thumb = cva_spread_approximation(
            in.lgd_counterparty * average_hazard,
            expected_positive_exposure(profile.times, profile.discounted_ee), maturity);

        // Euler allocation of the uncollateralised CVA: the trade's
        // contribution to EE*, integrated against the same default weights.
        std::vector<Real> marginal(trades.size(), std::numeric_limits<Real>::quiet_NaN());
        if (!in.csa && in.wrong_way_b == 0.0)
        {
            const ExposureStatistics by_trade = exposure_statistics(paths, trades, in.pfe_confidence);
            const std::vector<Real> weights = first_to_default_weights(by_trade.times, in.counterparty, in.own);
            for (std::size_t j = 0; j < trades.size(); ++j)
            {
                Real sum = 0.0;
                for (std::size_t i = 0; i < weights.size(); ++i)
                    sum += by_trade.discounted_ee_contributions[j][i] * weights[i];
                marginal[j] = -in.lgd_counterparty * sum;
            }
        }

        for (std::size_t j = 0; j < trades.size(); ++j)
        {
            TradeContribution c;
            c.trade = trades[j];
            c.standalone_cva = bilateral_cva(paths, in, {trades[j]}, margin_scaling);
            std::vector<std::size_t> others = trades;
            others.erase(others.begin() + static_cast<std::ptrdiff_t>(j));
            c.incremental_cva =
                report.cva.value -
                (others.empty() ? 0.0 : bilateral_cva(paths, in, others, margin_scaling));
            c.marginal_cva = marginal[j];
            report.contributions.push_back(c);
        }
        return report;
    }

} // namespace quantModeling
