#include "quantModeling/scripting/exercise.hpp"

#include <Eigen/Dense>

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

    double ExerciseRegression::gain(const std::vector<double> &regressors) const
    {
        std::vector<double> z(regressors.size());
        for (std::size_t i = 0; i < z.size(); ++i)
            z[i] = (regressors[i] - mean[i]) / scale[i];
        const std::vector<double> phi = basis(z, terms);
        double g = 0.0;
        for (std::size_t t = 0; t < phi.size(); ++t)
            g += beta[t] * phi[t];
        return g;
    }

    bool ExercisePolicy::decide(std::size_t event, const std::vector<double> &regressors) const
    {
        const auto it = by_event.find(event);
        if (it == by_event.end())
            return false;
        if (regressors.size() != it->second.mean.size())
            throw InvalidInput("exercise(): the number of regressors changed between the pilot and the pricing");
        const double g = it->second.gain(regressors);
        return issuer ? g < 0.0 : g > 0.0;
    }

    ExerciseRegression fit_exercise_regression(const std::vector<std::vector<double>> &rows,
                                               const std::vector<double> &targets, int degree)
    {
        if (rows.empty() || rows.size() != targets.size())
            throw InvalidInput("exercise regression: need as many targets as rows, and at least one");
        const std::size_t n = rows.front().size(), m = rows.size();
        ExerciseRegression r;
        r.degree = degree;
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
        const auto terms = monomials(n, degree);
        Eigen::MatrixXd A(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(terms.size()));
        Eigen::VectorXd b(static_cast<Eigen::Index>(m));
        std::vector<double> z(n);
        for (std::size_t p = 0; p < m; ++p)
        {
            for (std::size_t i = 0; i < n; ++i)
                z[i] = (rows[p][i] - r.mean[i]) / r.scale[i];
            const std::vector<double> phi = basis(z, terms);
            for (std::size_t t = 0; t < phi.size(); ++t)
                A(static_cast<Eigen::Index>(p), static_cast<Eigen::Index>(t)) = phi[t];
            b(static_cast<Eigen::Index>(p)) = targets[p];
        }
        const Eigen::VectorXd beta = A.colPivHouseholderQr().solve(b);
        r.beta.assign(beta.data(), beta.data() + beta.size());
        r.terms = terms;
        return r;
    }

} // namespace quantModeling::scripting
