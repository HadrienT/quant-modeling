#ifndef UTILS_VARIANCE_REDUCTION_MULTI_CONTROL_HPP
#define UTILS_VARIANCE_REDUCTION_MULTI_CONTROL_HPP

#include <cmath>
#include <cstddef>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

/**
 * @file multi_control.hpp
 * @brief Several control variates at once, by regression
 *        (Glasserman 2004, §4.1.2-4.1.3; blueprint/wp/19-gpu.md §2.5, lot G3).
 *
 * Per path: the target Y and up to kMaxControls controls X_c of known means
 * mu_c. The estimator is the regression one,
 *
 *     theta = Ybar - beta' (Xbar - mu),     beta = Sxx^-1 Sxy,
 *
 * with Sxx, Sxy the centred sums of squares and cross-products -- beta is the
 * least-squares slope of Y on X, estimated on the same paths (a bias of
 * order 1/n, negligible at Monte-Carlo sizes; Glasserman §4.1.3).
 *
 * The accumulator is a multivariate Welford (mean vector and centred
 * co-moment matrix of (Y, X), updated one path at a time) and merges by
 * Chan's formula, so it goes through the same logical-block reduction tree
 * as a plain Welford, on the CPU and in the GPU kernel: fixed size, no
 * allocation, trivially copyable.
 *
 * The standard error is the regression's own, at the point X = mu:
 *
 *     Var(theta) = s^2 (1/n + (Xbar - mu)' Sxx^-1 (Xbar - mu)),
 *     s^2 = (Syy - beta' Sxy) / (n - k - 1),
 *
 * the residual variance on n - k - 1 degrees of freedom. A control that is
 * (numerically) a combination of the others, or constant, is dropped by the
 * solve (its beta is 0).
 */

namespace quantModeling
{

    struct MultiControlAccumulator
    {
        static constexpr int kMaxControls = 8;
        static constexpr int kDim = kMaxControls + 1;         ///< (Y, X_1..X_k)
        static constexpr int kPacked = kDim * (kDim + 1) / 2; ///< upper triangle

        long long n = 0;
        Real mean[kDim] = {};
        Real co[kPacked] = {}; ///< sum (v_i - mean_i)(v_j - mean_j), i <= j

        QM_HOST_DEVICE static int idx(int i, int j)
        {
            // row-major upper triangle: row i starts after sum_{r<i} (kDim - r)
            return i * kDim - i * (i - 1) / 2 + (j - i);
        }

        /// One path's (Y, X_1..X_kMaxControls), unused controls 0.
        struct Sample
        {
            Real v[kDim] = {};
        };

        QM_HOST_DEVICE void add(const Sample &s) { add(s.v); }

        /// One path: v[0] = Y, v[1..kMaxControls] = X (unused controls 0).
        QM_HOST_DEVICE void add(const Real *v)
        {
            ++n;
            Real delta[kDim];
            for (int i = 0; i < kDim; ++i)
            {
                delta[i] = v[i] - mean[i];
                mean[i] += delta[i] / static_cast<Real>(n);
            }
            for (int i = 0; i < kDim; ++i)
            {
                const Real after = v[i] - mean[i];
                for (int j = i; j < kDim; ++j)
                    co[idx(i, j)] += after * delta[j]; // (v - mean_new)(v - mean_old)'
            }
        }

        /// Chan's parallel merge.
        QM_HOST_DEVICE void merge(const MultiControlAccumulator &o)
        {
            if (o.n == 0)
                return;
            if (n == 0)
            {
                *this = o;
                return;
            }
            const Real na = static_cast<Real>(n);
            const Real nb = static_cast<Real>(o.n);
            const Real total = na + nb;
            Real delta[kDim];
            for (int i = 0; i < kDim; ++i)
                delta[i] = o.mean[i] - mean[i];
            for (int i = 0; i < kDim; ++i)
                for (int j = i; j < kDim; ++j)
                    co[idx(i, j)] += o.co[idx(i, j)] + delta[i] * delta[j] * na * nb / total;
            for (int i = 0; i < kDim; ++i)
                mean[i] += delta[i] * nb / total;
            n += o.n;
        }

        struct Estimate
        {
            Real value = 0.0;
            Real std_error = 0.0;
            Real plain_std_error = 0.0; ///< Y alone, without the controls
            int used = 0;               ///< controls kept by the solve
            Real beta[kMaxControls] = {};
        };

        /**
         * @brief The regression estimate for k controls of means mu[0..k).
         *        k = 0 gives the plain mean.
         */
        Estimate estimate(const Real *mu, int k) const { return regress(co, mean, n, mu, k); }

        /**
         * @brief beta from the co-moments `c` (packed like co), applied to
         *        the sample means `m` of n paths; the error as for an i.i.d.
         *        sample. Stratified runs pass within-stratum co-moments and
         *        take their error from replicates instead.
         */
        static Estimate regress(const Real *c, const Real *m, long long n, const Real *mu, int k)
        {
            const Real *co = c;
            const Real *mean = m;
            Estimate e;
            const Real nn = static_cast<Real>(n);
            const Real syy = co[idx(0, 0)];
            e.value = mean[0];
            e.plain_std_error = n > 1 ? std::sqrt(syy / (nn - 1.0) / nn) : 0.0;
            e.std_error = e.plain_std_error;
            if (k <= 0 || n < k + 3)
                return e;

            // Cholesky of Sxx, dropping a control whose pivot vanishes (it
            // adds nothing the previous ones do not already carry).
            Real L[kMaxControls][kMaxControls] = {};
            bool keep[kMaxControls] = {};
            Real trace = 0.0;
            for (int i = 0; i < k; ++i)
                trace += co[idx(i + 1, i + 1)];
            const Real tol = 1e-12 * (trace > 0.0 ? trace : 1.0);
            for (int i = 0; i < k; ++i)
            {
                Real s = co[idx(i + 1, i + 1)];
                for (int p = 0; p < i; ++p)
                    if (keep[p])
                        s -= L[i][p] * L[i][p];
                if (s <= tol)
                    continue;
                keep[i] = true;
                L[i][i] = std::sqrt(s);
                for (int j = i + 1; j < k; ++j)
                {
                    Real c = co[idx(i + 1, j + 1)];
                    for (int p = 0; p < i; ++p)
                        if (keep[p])
                            c -= L[j][p] * L[i][p];
                    L[j][i] = c / L[i][i];
                }
            }

            // Solve Sxx a = b by forward and back substitution on the kept
            // controls (dropped ones are held at zero).
            auto solve = [&](const Real *b, Real *a)
            {
                Real y[kMaxControls] = {};
                for (int i = 0; i < k; ++i)
                {
                    if (!keep[i])
                        continue;
                    Real s = b[i];
                    for (int p = 0; p < i; ++p)
                        if (keep[p])
                            s -= L[i][p] * y[p];
                    y[i] = s / L[i][i];
                }
                for (int i = k - 1; i >= 0; --i)
                {
                    a[i] = 0.0;
                    if (!keep[i])
                        continue;
                    Real s = y[i];
                    for (int p = i + 1; p < k; ++p)
                        if (keep[p])
                            s -= L[p][i] * a[p];
                    a[i] = s / L[i][i];
                }
            };

            Real sxy[kMaxControls] = {};
            Real gap[kMaxControls] = {};
            for (int i = 0; i < k; ++i)
            {
                sxy[i] = co[idx(0, i + 1)];
                gap[i] = mean[i + 1] - mu[i];
                e.used += keep[i] ? 1 : 0;
            }
            if (e.used == 0 || n <= e.used + 1)
                return e;
            solve(sxy, e.beta);

            Real explained = 0.0, correction = 0.0;
            for (int i = 0; i < k; ++i)
            {
                explained += e.beta[i] * sxy[i];
                correction += e.beta[i] * gap[i];
            }
            Real w[kMaxControls] = {};
            solve(gap, w);
            Real leverage = 0.0; // (Xbar - mu)' Sxx^-1 (Xbar - mu)
            for (int i = 0; i < k; ++i)
                leverage += gap[i] * w[i];

            Real rss = syy - explained;
            if (rss < 0.0)
                rss = 0.0;
            const Real s2 = rss / (nn - static_cast<Real>(e.used) - 1.0);
            e.value = mean[0] - correction;
            e.std_error = std::sqrt(s2 * (1.0 / nn + leverage));
            return e;
        }
    };

    /**
     * @brief Control variates on a stratified sample
     *        (blueprint/wp/19-gpu.md §2.5).
     *
     * Stratification already removes the between-strata part of the
     * variance, so what a control can still remove is the *within*-stratum
     * part: the right slope is beta_w = Sxx,w^-1 Sxy,w, from within-stratum
     * co-moments. The ordinary regression slope, from total co-moments, is
     * the wrong one there (measured: a Phoenix autocall's variance / 3.4
     * stratified alone, / 2.3 with the ordinary slope).
     *
     * With one path per stratum there is no within-stratum spread to
     * measure directly; neighbouring strata are nearly alike, so successive
     * differences estimate it -- E[(v_{i+1} - v_i)(v_{i+1} - v_i)'] ~
     * 2 Sigma_w (the collapsed-strata / successive-differences estimator of
     * survey sampling; Cochran, Sampling Techniques, 3rd ed., Wiley 1977).
     * The caller feeds the paths of each thread in stratum order
     * (mc::stratum_of), so the differences are taken between a thread's
     * consecutive paths; merging adds them up.
     *
     * The estimate theta = Ybar - beta_w (Xbar - mu) of one replicate is
     * unbiased up to O(1/m) (beta_w is estimated on the same paths); its
     * error comes from independent replicates.
     */
    struct StratifiedControlAccumulator
    {
        using Sample = MultiControlAccumulator::Sample;
        static constexpr int kDim = MultiControlAccumulator::kDim;
        static constexpr int kPacked = MultiControlAccumulator::kPacked;

        MultiControlAccumulator all; ///< means (and total co-moments)
        Real diff[kPacked] = {};     ///< sum over successive pairs of d d', d = v_{i+1} - v_i
        long long n_diff = 0;
        Real last[kDim] = {}; ///< this thread's previous path (not merged)
        long long has_last = 0;

        QM_HOST_DEVICE void add(const Sample &s)
        {
            all.add(s);
            if (has_last)
            {
                Real d[kDim];
                for (int i = 0; i < kDim; ++i)
                    d[i] = s.v[i] - last[i];
                for (int i = 0; i < kDim; ++i)
                    for (int j = i; j < kDim; ++j)
                        diff[MultiControlAccumulator::idx(i, j)] += d[i] * d[j];
                ++n_diff;
            }
            for (int i = 0; i < kDim; ++i)
                last[i] = s.v[i];
            has_last = 1;
        }

        QM_HOST_DEVICE void merge(const StratifiedControlAccumulator &o)
        {
            all.merge(o.all);
            for (int i = 0; i < kPacked; ++i)
                diff[i] += o.diff[i];
            n_diff += o.n_diff;
            has_last = 0; // a merged partial has no single "previous path"
        }

        /// The replicate's estimate with the within-stratum slope.
        Real estimate(const Real *mu, int k) const
        {
            if (n_diff < k + 3)
                return all.mean[0];
            return MultiControlAccumulator::regress(diff, all.mean, n_diff, mu, k).value;
        }
    };

} // namespace quantModeling

#endif // UTILS_VARIANCE_REDUCTION_MULTI_CONTROL_HPP
