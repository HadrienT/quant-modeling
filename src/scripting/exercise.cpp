#include "quantModeling/scripting/exercise.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <functional>

namespace quantModeling::scripting
{

    std::vector<std::vector<int>> monomials(std::size_t n, int degree)
    {
        std::vector<std::vector<int>> out;
        std::vector<int> e(n, 0);
        // Every exponent vector of total degree ≤ degree, by increasing total
        // degree, then lexicographically: 1, z0, z1, z0², z0 z1, z1², ...
        for (int total = 0; total <= degree; ++total)
        {
            std::function<void(std::size_t, int)> fill = [&](std::size_t i, int left)
            {
                if (i + 1 == n || n == 0)
                {
                    if (n > 0)
                        e[i] = left;
                    if (n > 0 || left == 0)
                        out.push_back(e);
                    return;
                }
                for (int k = left; k >= 0; --k)
                {
                    e[i] = k;
                    fill(i + 1, left - k);
                }
            };
            fill(0, total);
        }
        return out;
    }

    std::vector<double> basis(const std::vector<double> &z, const std::vector<std::vector<int>> &terms)
    {
        std::vector<double> phi(terms.size(), 1.0);
        for (std::size_t t = 0; t < terms.size(); ++t)
            for (std::size_t i = 0; i < z.size(); ++i)
                for (int k = 0; k < terms[t][i]; ++k)
                    phi[t] *= z[i];
        return phi;
    }

    namespace
    {
        /// basis()'s products, for one monomial, without a vector.
        double monomial(const double *z, std::size_t n, const std::vector<int> &term)
        {
            double phi = 1.0;
            for (std::size_t i = 0; i < n; ++i)
                for (int k = 0; k < term[i]; ++k)
                    phi *= z[i];
            return phi;
        }
    } // namespace

    double ExerciseRegression::gain(const std::vector<double> &regressors) const
    {
        return gain(regressors.data(), regressors.size());
    }

    double ExerciseRegression::gain(const double *regressors, std::size_t n) const
    {
        // The same operations as basis() then the dot product, without
        // allocating: it runs for every pilot path at every date, and for
        // every decision the tree evaluator prices (WP 26 §17).
        constexpr std::size_t kSmall = 16;
        double small[kSmall];
        std::vector<double> large;
        double *z = small;
        if (n > kSmall)
        {
            large.resize(n);
            z = large.data();
        }
        for (std::size_t i = 0; i < n; ++i)
            z[i] = (regressors[i] - mean[i]) / scale[i];
        double g = 0.0;
        for (std::size_t t = 0; t < terms.size(); ++t)
            g += beta[t] * monomial(z, n, terms[t]);
        return g;
    }

    bool ExercisePolicy::decide(std::size_t event, const std::vector<double> &regressors) const
    {
        const auto it = by_event.find(event);
        if (it == by_event.end())
            return false;
        if (regressors.size() != it->second.mean.size())
            throw InvalidInput("exercise(): the number of regressors changed between the pilot and the pricing");
        const ExerciseRegression &reg = it->second;
        for (std::size_t i = 0; i < reg.lo.size() && i < regressors.size(); ++i)
            if (regressors[i] < reg.lo[i] || regressors[i] > reg.hi[i])
                return false; // outside where the rule was fitted
        const double g = reg.gain(regressors);
        return issuer ? g < 0.0 : g > 0.0;
    }

    namespace
    {
        /// Mean and scale of each regressor over the rows.
        void standardise(const std::vector<std::vector<double>> &rows, ExerciseRegression &r)
        {
            const std::size_t n = rows.front().size(), m = rows.size();
            r.mean.assign(n, 0.0);
            r.scale.assign(n, 1.0);
            for (const auto &row : rows)
                for (std::size_t i = 0; i < n; ++i)
                    r.mean[i] += row[i] / static_cast<double>(m);
            for (std::size_t i = 0; i < n; ++i)
            {
                double var = 0.0;
                for (const auto &row : rows)
                    var += (row[i] - r.mean[i]) * (row[i] - r.mean[i]);
                const double sd = std::sqrt(var / static_cast<double>(m));
                r.scale[i] = sd > 1e-12 * (1.0 + std::abs(r.mean[i])) ? sd : 1.0;
            }
        }

        /// standardise() on a row-major matrix of m rows of n: the same
        /// operations in the same order.
        void standardise(const double *rows, std::size_t m, std::size_t n, ExerciseRegression &r)
        {
            r.mean.assign(n, 0.0);
            r.scale.assign(n, 1.0);
            for (std::size_t p = 0; p < m; ++p)
                for (std::size_t i = 0; i < n; ++i)
                    r.mean[i] += rows[p * n + i] / static_cast<double>(m);
            for (std::size_t i = 0; i < n; ++i)
            {
                double var = 0.0;
                for (std::size_t p = 0; p < m; ++p)
                    var += (rows[p * n + i] - r.mean[i]) * (rows[p * n + i] - r.mean[i]);
                const double sd = std::sqrt(var / static_cast<double>(m));
                r.scale[i] = sd > 1e-12 * (1.0 + std::abs(r.mean[i])) ? sd : 1.0;
            }
        }

        /// β from the stacked rows A β = b.
        void solve(const Eigen::MatrixXd &A, const Eigen::VectorXd &b, ExerciseRegression &r)
        {
            // A rank threshold well above rounding: regressors collinear on the
            // fitted paths (the payoff and the spot of a put, in the money,
            // where max(K - S, 0) = K - S) leave pivots of 1e-14 that the default
            // threshold keeps, and dividing by them fills beta with cancelling
            // giants whose rounding is the prediction (issue #228).
            //
            // The threshold has to be applied here: Eigen's solve() keeps
            // every pivot above machine epsilon (nonZeroPivots()), and the
            // prescribed threshold only reaches rank(). Until 2026-11-17 the
            // rule of a Bermudan put on (max(K - S, 0), S) still carried
            // coefficients of 1e13-1e14 that cancelled, and decided by the
            // last bits of the spot (WP 26 §16). So: the first rank()
            // pivoted columns, R's leading block solved, the others 0.
            Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr;
            qr.setThreshold(1e-10);
            qr.compute(A);
            const Eigen::Index rank = qr.rank();
            Eigen::VectorXd c = b;
            c.applyOnTheLeft(qr.householderQ().setLength(rank).adjoint());
            Eigen::VectorXd y = Eigen::VectorXd::Zero(A.cols());
            if (rank > 0)
                y.head(rank) = qr.matrixR().topLeftCorner(rank, rank).triangularView<Eigen::Upper>().solve(c.head(rank));
            const Eigen::VectorXd beta = qr.colsPermutation() * y;
            r.beta.assign(beta.data(), beta.data() + beta.size());
        }

        double variance(const std::vector<double> &x)
        {
            double mean = 0.0, var = 0.0;
            for (const double v : x)
                mean += v / static_cast<double>(x.size());
            for (const double v : x)
                var += (v - mean) * (v - mean) / static_cast<double>(x.size());
            return var;
        }
    } // namespace

    ExerciseRegression fit_exercise_regression(const std::vector<std::vector<double>> &rows,
                                               const std::vector<double> &targets, int degree)
    {
        if (rows.empty() || rows.size() != targets.size())
            throw InvalidInput("exercise regression: need as many targets as rows, and at least one");
        const std::size_t n = rows.front().size();
        std::vector<double> flat;
        flat.reserve(rows.size() * n);
        for (const auto &row : rows)
            flat.insert(flat.end(), row.begin(), row.begin() + static_cast<std::ptrdiff_t>(n));
        return fit_exercise_regression(flat.data(), rows.size(), n, targets.data(), degree);
    }

    ExerciseRegression fit_exercise_regression(const double *rows, std::size_t m, std::size_t n,
                                               const double *targets, int degree)
    {
        if (m == 0)
            throw InvalidInput("exercise regression: need as many targets as rows, and at least one");
        ExerciseRegression r;
        r.degree = degree;
        standardise(rows, m, n, r);
        const auto terms = monomials(n, degree);
        Eigen::MatrixXd A(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(terms.size()));
        Eigen::VectorXd b(static_cast<Eigen::Index>(m));
        std::vector<double> z(n);
        for (std::size_t p = 0; p < m; ++p)
        {
            for (std::size_t i = 0; i < n; ++i)
                z[i] = (rows[p * n + i] - r.mean[i]) / r.scale[i];
            for (std::size_t t = 0; t < terms.size(); ++t)
                A(static_cast<Eigen::Index>(p), static_cast<Eigen::Index>(t)) = monomial(z.data(), n, terms[t]);
            b(static_cast<Eigen::Index>(p)) = targets[p];
        }
        solve(A, b, r);
        r.terms = terms;
        return r;
    }

    ExerciseRegression fit_differential_regression(const std::vector<std::vector<double>> &rows,
                                                   const std::vector<double> &targets,
                                                   const std::vector<double> &derivatives, int degree,
                                                   double lambda)
    {
        if (rows.empty() || rows.size() != targets.size() || derivatives.size() != targets.size())
            throw InvalidInput("differential regression: need as many targets and derivatives as rows");
        if (!(lambda >= 0.0) || !std::isfinite(lambda))
            throw InvalidInput("differential regression: the weight of the derivatives must be >= 0");
        // λ₀ = Var(y) / Var(z); a slope that hardly varies (a swap's, in a
        // narrow bucket) is information all the same: its variance is
        // floored at a millionth of its mean square. Derivatives all zero (an
        // expired option) and targets that do not vary leave the plain fit.
        double square_z = 0.0;
        for (const double v : derivatives)
            square_z += v * v / static_cast<double>(derivatives.size());
        if (lambda == 0.0 || !(square_z > 0.0))
            return fit_exercise_regression(rows, targets, degree);
        const double weight = std::sqrt(lambda * variance(targets) / std::max(variance(derivatives), 1e-6 * square_z));
        if (!(weight > 0.0) || !std::isfinite(weight))
            return fit_exercise_regression(rows, targets, degree);

        const std::size_t n = rows.front().size(), m = rows.size();
        ExerciseRegression r;
        r.degree = degree;
        standardise(rows, r);
        const auto terms = monomials(n, degree);
        const auto T = static_cast<Eigen::Index>(terms.size());
        Eigen::MatrixXd A(static_cast<Eigen::Index>(2 * m), T);
        Eigen::VectorXd b(static_cast<Eigen::Index>(2 * m));
        std::vector<double> z(n);
        for (std::size_t p = 0; p < m; ++p)
        {
            for (std::size_t i = 0; i < n; ++i)
                z[i] = (rows[p][i] - r.mean[i]) / r.scale[i];
            const auto row = static_cast<Eigen::Index>(p), drow = static_cast<Eigen::Index>(m + p);
            for (Eigen::Index t = 0; t < T; ++t)
            {
                const std::vector<int> &e = terms[static_cast<std::size_t>(t)];
                double phi = 1.0, dphi = 0.0;
                // d/dx0 of Π z_k^e_k, with z0 = (x0 - mean) / scale.
                if (e[0] > 0)
                {
                    dphi = static_cast<double>(e[0]) / r.scale[0];
                    for (int k = 1; k < e[0]; ++k)
                        dphi *= z[0];
                }
                for (std::size_t i = 0; i < n; ++i)
                    for (int k = 0; k < e[i]; ++k)
                        phi *= z[i];
                for (std::size_t i = 1; i < n; ++i)
                    for (int k = 0; k < e[i]; ++k)
                        dphi *= z[i];
                A(row, t) = phi;
                A(drow, t) = weight * dphi;
            }
            b(row) = targets[p];
            b(drow) = weight * derivatives[p];
        }
        solve(A, b, r);
        r.terms = terms;
        return r;
    }

} // namespace quantModeling::scripting
