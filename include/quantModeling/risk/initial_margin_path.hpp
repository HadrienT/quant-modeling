#ifndef QM_RISK_INITIAL_MARGIN_PATH_HPP
#define QM_RISK_INITIAL_MARGIN_PATH_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{

    /**
     * @file initial_margin_path.hpp
     * @brief A fitted dynamic initial margin (risk/initial_margin.hpp) as
     *        plain arrays, and its evaluation on one path -- on the host or
     *        in a kernel (#281 step 4, lot G8 of blueprint/wp/23-xva.md §20).
     *
     * Per reporting date r, with V the netting set's value at the lagged
     * date t_r - MPoR:
     *
     *   IM(t_r) = s (|μ_r(V)| + q sqrt(σ²_r(V)))      (σ²_r(V) <= 0: its average)
     *
     * and the scaled margin of today where the lagged date is today. μ_r
     * and σ²_r are regressions by buckets on V, each bucket a polynomial
     * in the standardised value z = (V - mean) / scale -- what
     * BucketedRegression evaluates, in the same order of operations.
     */

    /// The fitted model, flattened. DynamicInitialMargin::table() fills it.
    struct InitialMarginTable
    {
        /// Per reporting date: its index on the grid, that of its lagged
        /// date (-1: today), the first bucket and the bucket count of the
        /// mean's regression and of the variance's, and the residuals'
        /// average square.
        std::vector<int> date, lagged;
        std::vector<int> mean_first, mean_buckets, variance_first, variance_buckets;
        std::vector<Real> average_variance;
        /// Per bucket: its upper edge (the last one's is never read), the
        /// standardisation of the value, its degree and its terms.
        std::vector<Real> edge, center, scale;
        std::vector<int> degree, term_first, terms;
        /// Per term: the power of z and its coefficient.
        std::vector<int> power;
        std::vector<Real> beta;
        /// The scaled margin in place today, the scaling and the quantile q.
        Real today = 0.0;
        Real scaling = 1.0;
        Real quantile = 0.0;

        std::size_t dates() const { return date.size(); }
    };

    /// The same arrays as pointers, for a kernel (or the host).
    struct InitialMarginView
    {
        const int *lagged = nullptr;
        const int *mean_first = nullptr, *mean_buckets = nullptr, *variance_first = nullptr,
                  *variance_buckets = nullptr;
        const Real *average_variance = nullptr;
        const Real *edge = nullptr, *center = nullptr, *scale = nullptr;
        const int *degree = nullptr, *term_first = nullptr, *terms = nullptr;
        const int *power = nullptr;
        const Real *beta = nullptr;
        Real today = 0.0;
        Real scaling = 1.0;
        Real quantile = 0.0;
    };

    inline InitialMarginView view(const InitialMarginTable &t)
    {
        return {t.lagged.data(),
                t.mean_first.data(),
                t.mean_buckets.data(),
                t.variance_first.data(),
                t.variance_buckets.data(),
                t.average_variance.data(),
                t.edge.data(),
                t.center.data(),
                t.scale.data(),
                t.degree.data(),
                t.term_first.data(),
                t.terms.data(),
                t.power.data(),
                t.beta.data(),
                t.today,
                t.scaling,
                t.quantile};
    }

    /// Regression by buckets at `v`: the first bucket whose edge is above
    /// v (std::upper_bound's), then its polynomial; no bucket predicts 0.
    QM_HOST_DEVICE inline Real bucketed_value(const InitialMarginView &m, int first, int buckets, Real v)
    {
        if (buckets == 0)
            return 0.0;
        int b = first;
        while (b < first + buckets - 1 && !(v < m.edge[b]))
            ++b;
        const int count = m.terms[b];
        if (count == 0)
            return 0.0;
        // The powers of z, then one product per term, as
        // PolynomialRegression does (its products by 1.0 are exact).
        Real power[8];
        const Real z = (v - m.center[b]) / m.scale[b];
        power[0] = 1.0;
        for (int d = 1; d <= m.degree[b]; ++d)
            power[d] = power[d - 1] * z;
        Real value = 0.0;
        for (int t = 0; t < count; ++t)
        {
            const int at = m.term_first[b] + t;
            value = value + m.beta[at] * power[m.power[at]];
        }
        return value;
    }

    /// The margin in place at reporting date r of a path whose netting set
    /// is worth `value_lagged` at the lagged date (unread when it is today).
    QM_HOST_DEVICE inline Real initial_margin_at(const InitialMarginView &m, int r, Real value_lagged)
    {
        if (m.lagged[r] < 0)
            return m.today;
        const Real fitted = bucketed_value(m, m.variance_first[r], m.variance_buckets[r], value_lagged);
        return m.scaling * (fabs(bucketed_value(m, m.mean_first[r], m.mean_buckets[r], value_lagged)) +
                            m.quantile * sqrt(fitted > 0.0 ? fitted : m.average_variance[r]));
    }

} // namespace quantModeling

#endif // QM_RISK_INITIAL_MARGIN_PATH_HPP
