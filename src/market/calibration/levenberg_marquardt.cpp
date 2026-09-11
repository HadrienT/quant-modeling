#include "quantModeling/market/calibration/levenberg_marquardt.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace quantModeling::calibration
{

    namespace
    {

        Real clamp_to_bounds(Real x, Real lo, Real hi)
        {
            return std::min(std::max(x, lo), hi);
        }

        std::vector<Real> clamp_params(std::vector<Real> params,
                                       const std::vector<Real> &lower,
                                       const std::vector<Real> &upper)
        {
            for (std::size_t j = 0; j < params.size(); ++j)
                params[j] = clamp_to_bounds(params[j], lower[j], upper[j]);
            return params;
        }

        Real weighted_cost(const std::vector<Real> &r, const std::vector<Real> &w)
        {
            Real cost = 0.0;
            for (std::size_t i = 0; i < r.size(); ++i)
                cost += w[i] * r[i] * r[i];
            return cost;
        }

        Real max_abs(const std::vector<Real> &v)
        {
            Real m = 0.0;
            for (const Real x : v)
                m = std::max(m, std::abs(x));
            return m;
        }

        /// Central-difference Jacobian (residuals x params), unweighted.
        Eigen::MatrixXd numerical_jacobian(const ObjectiveFunction &objective,
                                           const std::vector<Real> &params,
                                           const std::vector<Real> &lower,
                                           const std::vector<Real> &upper,
                                           Real fd_step)
        {
            const auto m = static_cast<Eigen::Index>(objective.num_residuals());
            const auto n = static_cast<Eigen::Index>(objective.num_params());
            Eigen::MatrixXd J(m, n);

            for (Eigen::Index j = 0; j < n; ++j)
            {
                const auto jj = static_cast<std::size_t>(j);
                const Real h = fd_step * std::max(Real(1.0), std::abs(params[jj]));

                std::vector<Real> p_up = params;
                std::vector<Real> p_dn = params;
                p_up[jj] = clamp_to_bounds(params[jj] + h, lower[jj], upper[jj]);
                p_dn[jj] = clamp_to_bounds(params[jj] - h, lower[jj], upper[jj]);
                const Real denom = p_up[jj] - p_dn[jj];

                const std::vector<Real> r_up = objective.residuals(p_up);
                const std::vector<Real> r_dn = objective.residuals(p_dn);

                for (Eigen::Index i = 0; i < m; ++i)
                {
                    const auto ii = static_cast<std::size_t>(i);
                    J(i, j) = (denom != 0.0) ? (r_up[ii] - r_dn[ii]) / denom : Real(0.0);
                }
            }
            return J;
        }

    } // namespace

    CalibrationReport levenberg_marquardt(
        const ObjectiveFunction &objective,
        std::vector<Real> initial_params,
        const LevenbergMarquardtSettings &settings)
    {
        const auto start = std::chrono::steady_clock::now();

        const std::size_t n = objective.num_params();
        const std::size_t m = objective.num_residuals();
        const std::vector<Real> lower = objective.lower_bounds();
        const std::vector<Real> upper = objective.upper_bounds();
        const std::vector<Real> w = objective.weights();

        std::vector<Real> params = clamp_params(std::move(initial_params), lower, upper);
        std::vector<Real> r = objective.residuals(params);
        Real cost = weighted_cost(r, w);

        Real lambda = settings.initial_lambda;
        bool converged = false;
        std::size_t iterations_done = 0;

        for (std::size_t iter = 0; iter < settings.max_iterations; ++iter)
        {
            ++iterations_done;

            const Eigen::MatrixXd J = numerical_jacobian(objective, params, lower, upper, settings.fd_step);

            Eigen::VectorXd rw(static_cast<Eigen::Index>(m));
            Eigen::MatrixXd Jw = J;
            for (std::size_t i = 0; i < m; ++i)
            {
                const Real sw = std::sqrt(w[i]);
                rw(static_cast<Eigen::Index>(i)) = sw * r[i];
                Jw.row(static_cast<Eigen::Index>(i)) *= sw;
            }

            const Eigen::MatrixXd JTJ = Jw.transpose() * Jw;
            const Eigen::VectorXd JTr = Jw.transpose() * rw;

            if (JTr.lpNorm<Eigen::Infinity>() < settings.gradient_tol)
            {
                converged = true;
                break;
            }

            Eigen::VectorXd diag = JTJ.diagonal();
            for (Eigen::Index j = 0; j < diag.size(); ++j)
                diag(j) = std::max(diag(j), Real(1e-15));

            bool step_accepted = false;
            for (std::size_t retry = 0; retry < settings.max_lambda_retries && !step_accepted; ++retry)
            {
                Eigen::MatrixXd augmented = JTJ;
                for (Eigen::Index j = 0; j < augmented.rows(); ++j)
                    augmented(j, j) += lambda * diag(j);

                const Eigen::VectorXd delta = augmented.ldlt().solve(-JTr);

                std::vector<Real> candidate(n);
                Real step_inf = 0.0;
                for (std::size_t j = 0; j < n; ++j)
                {
                    const Real raw = params[j] + delta(static_cast<Eigen::Index>(j));
                    candidate[j] = clamp_to_bounds(raw, lower[j], upper[j]);
                    step_inf = std::max(step_inf, std::abs(candidate[j] - params[j]));
                }

                const std::vector<Real> r_candidate = objective.residuals(candidate);
                const Real candidate_cost = weighted_cost(r_candidate, w);

                if (candidate_cost < cost)
                {
                    const Real rel_decrease = (cost > 0.0) ? (cost - candidate_cost) / cost : 0.0;

                    params = candidate;
                    r = r_candidate;
                    cost = candidate_cost;
                    lambda *= settings.lambda_down;
                    step_accepted = true;

                    const Real params_inf = std::max(Real(1.0), max_abs(params));
                    if (step_inf < settings.step_tol * params_inf || rel_decrease < settings.cost_tol)
                        converged = true;
                }
                else
                {
                    lambda *= settings.lambda_up;
                }
            }

            if (!step_accepted || converged)
                break;
        }

        CalibrationReport report;
        report.params = params;
        report.iterations = iterations_done;
        report.converged = converged;

        Real sum_sq = 0.0;
        Real worst = 0.0;
        for (const Real ri : r)
        {
            sum_sq += ri * ri;
            worst = std::max(worst, std::abs(ri));
        }
        report.rmse = (m > 0) ? std::sqrt(sum_sq / static_cast<Real>(m)) : Real(0.0);
        report.worst_residual = worst;

        const auto end = std::chrono::steady_clock::now();
        report.wall_time_seconds = std::chrono::duration<double>(end - start).count();

        return report;
    }

} // namespace quantModeling::calibration
