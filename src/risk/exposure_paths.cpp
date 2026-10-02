#include "quantModeling/risk/exposure_paths.hpp"

#include "quantModeling/risk/exposure_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>
#include <utility>

namespace quantModeling
{
    namespace
    {
        /// Mean and standard error of the mean from Σ y and Σ y².
        std::pair<Real, Real> mean_and_error(Real sum, Real sum_squares, std::size_t n)
        {
            const Real count = static_cast<Real>(n);
            const Real mean = sum / count;
            if (n < 2)
                return {mean, 0.0};
            const Real variance = std::max(sum_squares / count - mean * mean, 0.0) * count / (count - 1.0);
            return {mean, std::sqrt(variance / count)};
        }
    } // namespace

    ExposureProfile ExposureStatistics::profile() const
    {
        if (measure != ExposureMeasure::RiskNeutral)
            throw InvalidInput("exposure profile: xVA is a price and integrates the risk-neutral "
                               "exposure; this simulation ran under the historical measure");
        return {times, discounted_ee, discounted_ene};
    }

    ExposureStatistics exposure_statistics(const ExposurePaths &paths,
                                           const std::vector<std::size_t> &trades,
                                           Real pfe_confidence)
    {
        const std::size_t n = paths.dates();
        const std::size_t N = paths.paths;
        if (n == 0 || N == 0 || paths.trades() == 0)
            throw InvalidInput("exposure statistics: the simulation is empty");
        if (paths.discount.size() != n || paths.discount_weight.size() != N * n)
            throw InvalidInput("exposure statistics: inconsistent simulation sizes");
        if (!(pfe_confidence > 0.0 && pfe_confidence < 1.0))
            throw InvalidInput("PFE confidence level must be in (0, 1)");

        ExposureStatistics s;
        s.times = paths.times;
        s.pfe_confidence = pfe_confidence;
        s.measure = paths.measure;
        s.trades = trades;
        if (s.trades.empty())
        {
            s.trades.resize(paths.trades());
            std::iota(s.trades.begin(), s.trades.end(), std::size_t{0});
        }
        std::vector<bool> seen(paths.trades(), false);
        for (const std::size_t k : s.trades)
        {
            if (k >= paths.trades())
                throw InvalidInput("exposure statistics: trade index out of range");
            if (seen[k])
                throw InvalidInput("exposure statistics: a trade is netted twice");
            if (paths.trade_values[k].size() != N * n)
                throw InvalidInput("exposure statistics: inconsistent simulation sizes");
            seen[k] = true;
            s.value_today += paths.trade_values_today[k];
        }

        // Close-out netting: the values add up before the positive part.
        std::vector<Real> netted(N * n, 0.0);
        for (const std::size_t k : s.trades)
        {
            const std::vector<Real> &values = paths.trade_values[k];
            for (std::size_t j = 0; j < N * n; ++j)
                netted[j] += values[j];
        }

        s.discounted_ee.resize(n);
        s.discounted_ene.resize(n);
        s.discounted_efv.resize(n);
        s.discounted_ee_error.resize(n);
        s.discounted_ene_error.resize(n);
        s.discounted_efv_error.resize(n);
        s.ee.resize(n);
        s.ene.resize(n);
        s.efv.resize(n);
        s.pfe.resize(n);
        s.discounted_ee_contributions.assign(s.trades.size(), std::vector<Real>(n, 0.0));

        std::vector<std::pair<Real, Real>> order(N);
        for (std::size_t i = 0; i < n; ++i)
        {
            Real ee = 0.0, ee2 = 0.0, ene = 0.0, ene2 = 0.0, efv = 0.0, efv2 = 0.0, weights = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real w = paths.discount_weight[p * n + i];
                const Real v = w * netted[p * n + i];
                const Real positive = std::max(v, 0.0), negative = std::min(v, 0.0);
                ee += positive;
                ee2 += positive * positive;
                ene += negative;
                ene2 += negative * negative;
                efv += v;
                efv2 += v * v;
                weights += w;
            }
            std::tie(s.discounted_ee[i], s.discounted_ee_error[i]) = mean_and_error(ee, ee2, N);
            std::tie(s.discounted_ene[i], s.discounted_ene_error[i]) = mean_and_error(ene, ene2, N);
            std::tie(s.discounted_efv[i], s.discounted_efv_error[i]) = mean_and_error(efv, efv2, N);
            s.ee[i] = s.discounted_ee[i] / paths.discount[i];
            s.ene[i] = s.discounted_ene[i] / paths.discount[i];
            s.efv[i] = s.discounted_efv[i] / paths.discount[i];

            // Euler allocation: E[D V_k 1{V_NS > 0}].
            for (std::size_t j = 0; j < s.trades.size(); ++j)
            {
                const std::vector<Real> &values = paths.trade_values[s.trades[j]];
                Real sum = 0.0;
                for (std::size_t p = 0; p < N; ++p)
                    if (netted[p * n + i] > 0.0)
                        sum += paths.discount_weight[p * n + i] * values[p * n + i];
                s.discounted_ee_contributions[j][i] = sum / static_cast<Real>(N);
            }

            // Quantile under the t_i-forward measure: each path counts for
            // its discount weight (the change of measure from the simulation
            // measure), normalised. The values are sorted with their weights:
            // paths of equal value give the same quantile in any order.
            for (std::size_t p = 0; p < N; ++p)
                order[p] = {netted[p * n + i], paths.discount_weight[p * n + i]};
            std::sort(order.begin(), order.end(),
                      [](const std::pair<Real, Real> &a, const std::pair<Real, Real> &b)
                      { return a.first < b.first; });
            const Real target = pfe_confidence * weights;
            Real cumulative = 0.0;
            Real quantile = order.back().first;
            for (const auto &[value, weight] : order)
            {
                cumulative += weight;
                if (cumulative >= target)
                {
                    quantile = value;
                    break;
                }
            }
            s.pfe[i] = std::max(quantile, 0.0);
        }

        s.epe = expected_positive_exposure(s.times, s.ee);
        s.eepe = effective_expected_positive_exposure(s.times, s.ee);
        return s;
    }

    ExposureProfile exposure_profile(const ExposurePaths &paths, const std::vector<std::size_t> &trades)
    {
        const std::size_t n = paths.dates();
        const std::size_t N = paths.paths;
        if (n == 0 || N == 0 || paths.trades() == 0)
            throw InvalidInput("exposure statistics: the simulation is empty");
        if (paths.discount_weight.size() != N * n)
            throw InvalidInput("exposure statistics: inconsistent simulation sizes");
        if (paths.measure != ExposureMeasure::RiskNeutral)
            throw InvalidInput("exposure profile: xVA is a price and integrates the risk-neutral "
                               "exposure; this simulation ran under the historical measure");
        std::vector<std::size_t> netted_trades = trades;
        if (netted_trades.empty())
        {
            netted_trades.resize(paths.trades());
            std::iota(netted_trades.begin(), netted_trades.end(), std::size_t{0});
        }
        std::vector<bool> seen(paths.trades(), false);
        for (const std::size_t k : netted_trades)
        {
            if (k >= paths.trades())
                throw InvalidInput("exposure statistics: trade index out of range");
            if (seen[k])
                throw InvalidInput("exposure statistics: a trade is netted twice");
            if (paths.trade_values[k].size() != N * n)
                throw InvalidInput("exposure statistics: inconsistent simulation sizes");
            seen[k] = true;
        }

        ExposureProfile profile;
        profile.times = paths.times;
        profile.discounted_ee.resize(n);
        profile.discounted_ene.resize(n);
        const Real count = static_cast<Real>(N);
        // One trade is its own netting: no copy.
        std::vector<Real> sum;
        if (netted_trades.size() > 1)
        {
            sum.assign(N * n, 0.0);
            for (const std::size_t k : netted_trades)
            {
                const std::vector<Real> &values = paths.trade_values[k];
                for (std::size_t j = 0; j < N * n; ++j)
                    sum[j] += values[j];
            }
        }
        const std::vector<Real> &netted =
            netted_trades.size() > 1 ? sum : paths.trade_values[netted_trades.front()];
        // Row by row: the matrices are read in the order they are stored.
        std::vector<Real> ee(n, 0.0), ene(n, 0.0);
        for (std::size_t p = 0; p < N; ++p)
            for (std::size_t i = 0; i < n; ++i)
            {
                const Real v = paths.discount_weight[p * n + i] * netted[p * n + i];
                ee[i] += std::max(v, 0.0);
                ene[i] += std::min(v, 0.0);
            }
        for (std::size_t i = 0; i < n; ++i)
        {
            profile.discounted_ee[i] = ee[i] / count;
            profile.discounted_ene[i] = ene[i] / count;
        }
        return profile;
    }

} // namespace quantModeling
