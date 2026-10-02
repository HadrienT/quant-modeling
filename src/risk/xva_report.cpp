#include "quantModeling/risk/xva_report.hpp"

#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/xva.hpp"

#include <algorithm>
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

        /// PD weights of the first-to-default sums, per date:
        /// S_other(t_{i-1}) (S_defaulter(t_{i-1}) - S_defaulter(t_i)).
        std::vector<Real> default_weights(const std::vector<Time> &times, const CreditCurve &defaulter,
                                          const CreditCurve &other)
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

        /// CVA and DVA of a single-trade cube, path by path: the mean of the
        /// per-path losses is the integral of the mean exposure, and their
        /// dispersion gives the Monte-Carlo error, correlations between
        /// dates included.
        void pathwise_adjustments(const ExposurePaths &cube, const XvaInputs &in, Estimate &cva,
                                  Estimate &dva)
        {
            const std::size_t n = cube.dates(), N = cube.paths;
            const std::vector<Real> pd_counterparty = default_weights(cube.times, in.counterparty, in.own);
            const std::vector<Real> pd_own = default_weights(cube.times, in.own, in.counterparty);
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

        /// Bilateral CVA of a subset of the trades, under the same CSA and,
        /// when there is one, its own initial margin.
        Real bilateral_cva(const ExposurePaths &paths, const XvaInputs &in,
                           const std::vector<std::size_t> &trades, Real margin_scaling)
        {
            ExposureStatistics s;
            if (in.initial_margin)
            {
                const InitialMargin margin = initial_margin(paths, in, trades, margin_scaling);
                s = exposure_statistics(netting_set_cube(paths, in, trades, &margin));
            }
            else
                s = exposure_statistics(netting_set_cube(paths, in, trades));
            return cva_bilateral(s.profile(), in.counterparty, in.own, in.lgd_counterparty);
        }
    } // namespace

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
        if (in.capital && in.capital->trades.size() != trades.size())
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
            const DynamicInitialMargin model = DynamicInitialMargin::fit(
                paths, in.csa->margin_period_of_risk, trades, *in.initial_margin);
            margin_scaling = model.scaling();
            margin = model.margin(paths);
            report.initial_margin_today = margin->today;
            report.expected_initial_margin = margin->expected;
            report.mva = mva(margin->times, margin->discounted_expected, in.counterparty, in.own,
                             in.initial_margin_spread);
        }
        const ExposurePaths cube =
            margin ? netting_set_cube(paths, in, trades, &*margin) : variation_only;
        report.exposure = exposure_statistics(cube, {}, in.pfe_confidence);
        const ExposureProfile profile = report.exposure.profile();

        pathwise_adjustments(cube, in, report.cva, report.dva);
        report.cva_unilateral = cva_unilateral(profile, in.counterparty, in.lgd_counterparty);
        // Segregated initial margin funds nothing: FVA is on the cube
        // without it.
        const ExposureProfile funding =
            margin ? exposure_statistics(variation_only).profile() : profile;
        report.fca = fca(funding, in.counterparty, in.own, in.borrowing_spread);
        report.fba = fba(funding, in.counterparty, in.own, in.lending_spread);
        if (in.csa)
            report.colva = colva(variation_only.times,
                                 discounted_expected_collateral(paths, *in.csa, trades),
                                 in.counterparty, in.own, in.collateral_spread);
        if (in.capital)
        {
            report.capital =
                projected_capital(variation_only, *in.capital, margin ? &*margin : nullptr);
            report.kva = kva(report.capital->times, report.capital->discounted_capital,
                             in.counterparty, in.own, in.cost_of_capital);
        }

        const Time maturity = profile.times.back();
        const Real average_hazard = -std::log(in.counterparty.survival(maturity)) / maturity;
        report.cva_rule_of_thumb = cva_spread_approximation(
            in.lgd_counterparty * average_hazard,
            expected_positive_exposure(profile.times, profile.discounted_ee), maturity);

        // Euler allocation of the uncollateralised CVA: the trade's
        // contribution to EE*, integrated against the same default weights.
        std::vector<Real> marginal(trades.size(), std::numeric_limits<Real>::quiet_NaN());
        if (!in.csa)
        {
            const ExposureStatistics by_trade = exposure_statistics(paths, trades, in.pfe_confidence);
            const std::vector<Real> weights = default_weights(by_trade.times, in.counterparty, in.own);
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
