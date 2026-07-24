#ifndef UTILS_VARIANCE_REDUCTION_CONTROL_VARIATE_HPP
#define UTILS_VARIANCE_REDUCTION_CONTROL_VARIATE_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/utils/accumulators.hpp"

namespace quantModeling
{

    /**
     * @brief Single-control control-variate accumulator.
     *
     * Given per-path pairs (Y, X) where Y is the target payoff and X a
     * control with known expectation E[X], the CV estimator is
     *
     *     Ŷ_cv = mean(Y) − β ( mean(X) − E[X] ),   β* = Cov(X,Y) / Var(X)
     *
     * which is unbiased for any fixed β and attains variance
     * Var(Y)(1 − ρ²) at β*. β is estimated on the same samples (bivariate
     * Welford co-moment); the induced O(1/n) bias is negligible and
     * vanishes entirely when batching (RQMC) treats each batch estimate
     * as one sample.
     *
     * Example (arithmetic Asian): X = discounted geometric-Asian payoff on
     * the same path, E[X] = discrete Kemna-Vorst closed form, ρ ≈ 0.99+.
     */
    struct ControlVariateAccumulator
    {
        WelfordAccumulator y; ///< target
        WelfordAccumulator x; ///< control
        Real comoment = 0.0;  ///< Σ (y_i − ȳ_i)(x_i − x̄_{i-1}) (bivariate Welford)

        QM_HOST_DEVICE void add(Real y_val, Real x_val)
        {
            // Bivariate Welford: use the *old* mean of x and *new* mean of y.
            const Real dx_old = x_val - x.mean;
            y.add(y_val);
            x.add(x_val);
            comoment += (y_val - y.mean) * dx_old;
        }

        QM_HOST_DEVICE Real covariance() const
        {
            return (y.n > 1) ? comoment / static_cast<Real>(y.n - 1) : Real(0);
        }

        /// Optimal β estimate (0 if the control is degenerate).
        QM_HOST_DEVICE Real beta() const
        {
            const Real vx = x.variance();
            return (vx > Real(0)) ? covariance() / vx : Real(0);
        }

        /// CV point estimate given the exact control expectation.
        QM_HOST_DEVICE Real estimate(Real expected_x) const
        {
            return y.mean - beta() * (x.mean - expected_x);
        }

        /// Estimated variance of a *single* CV-adjusted sample:
        /// Var(Y)(1 − ρ²). Divide by n for the variance of the estimator.
        QM_HOST_DEVICE Real reduced_variance() const
        {
            const Real vy = y.variance();
            const Real vx = x.variance();
            if (vy <= Real(0) || vx <= Real(0))
                return vy;
            const Real c = covariance();
            const Real rho2 = (c * c) / (vx * vy);
            return vy * (Real(1) - (rho2 < Real(1) ? rho2 : Real(1)));
        }
    };

} // namespace quantModeling

#endif // UTILS_VARIANCE_REDUCTION_CONTROL_VARIATE_HPP
