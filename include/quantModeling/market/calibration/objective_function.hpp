#ifndef MARKET_CALIBRATION_OBJECTIVE_FUNCTION_HPP
#define MARKET_CALIBRATION_OBJECTIVE_FUNCTION_HPP

#include "quantModeling/core/types.hpp"

#include <limits>
#include <vector>

namespace quantModeling::calibration
{

    /**
     * A weighted nonlinear least-squares calibration target:
     *
     *   minimize_params  sum_i  weights()[i] * residuals(params)[i]^2
     *
     * Express residuals() in whatever unit the calibration report should read
     * in — for a smile calibration that is implied-vol points
     * (model_iv(params) - market_iv), not price, so that CalibrationReport::rmse
     * comes out in vol points without any further conversion. weights() only
     * shapes the optimization path (e.g. vega-weighting so the wings don't
     * dominate the ATM fit); it does not change what unit the report is in.
     */
    struct ObjectiveFunction
    {
        virtual ~ObjectiveFunction() = default;

        virtual std::size_t num_params() const = 0;
        virtual std::size_t num_residuals() const = 0;

        virtual std::vector<Real> residuals(const std::vector<Real> &params) const = 0;

        /// Per-residual weight. Uniform by default.
        virtual std::vector<Real> weights() const
        {
            return std::vector<Real>(num_residuals(), 1.0);
        }

        virtual std::vector<Real> lower_bounds() const
        {
            return std::vector<Real>(num_params(), -std::numeric_limits<Real>::infinity());
        }

        virtual std::vector<Real> upper_bounds() const
        {
            return std::vector<Real>(num_params(), std::numeric_limits<Real>::infinity());
        }
    };

} // namespace quantModeling::calibration

#endif
