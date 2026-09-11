#ifndef MARKET_CALIBRATION_LEVENBERG_MARQUARDT_HPP
#define MARKET_CALIBRATION_LEVENBERG_MARQUARDT_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/calibration/objective_function.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling::calibration
{

    struct LevenbergMarquardtSettings
    {
        std::size_t max_iterations = 200;
        std::size_t max_lambda_retries = 30; ///< per-iteration retries before giving up on that step
        Real initial_lambda = 1e-3;
        Real lambda_up = 10.0;
        Real lambda_down = 0.1;
        Real gradient_tol = 1e-10; ///< convergence on ||J^T W r||_inf
        Real step_tol = 1e-12;     ///< convergence on ||delta params||_inf, relative to ||params||_inf
        Real cost_tol = 1e-14;     ///< convergence on relative cost decrease
        Real fd_step = 1e-6;       ///< relative step for the central-difference Jacobian
    };

    struct CalibrationReport
    {
        std::vector<Real> params;
        std::size_t iterations = 0;
        Real rmse = 0.0;           ///< unweighted RMS of residuals(), in whatever unit residuals() returns
        Real worst_residual = 0.0; ///< max |residual|, unweighted
        bool converged = false;
        double wall_time_seconds = 0.0;
    };

    /**
     * Levenberg-Marquardt on a weighted nonlinear least-squares ObjectiveFunction.
     *
     * Box constraints are enforced by clamping every accepted step to
     * [lower_bounds(), upper_bounds()] (projected LM). That is a simplification
     * — a proper active-set or interior treatment would handle bounds more
     * carefully — but it is standard practice for the parameter counts this
     * framework targets (a handful of smile parameters per slice, not
     * thousands), and it composes with any objective without extra machinery.
     *
     * The Jacobian is central finite differences; there is no analytic
     * gradient path yet because nothing in the tree is differentiable end to
     * end before WP17 (AAD). This is the numerical-Jacobian oracle that a
     * future AAD-based Jacobian must reproduce to machine precision.
     */
    CalibrationReport levenberg_marquardt(
        const ObjectiveFunction &objective,
        std::vector<Real> initial_params,
        const LevenbergMarquardtSettings &settings = {});

} // namespace quantModeling::calibration

#endif
