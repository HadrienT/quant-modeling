#ifndef QM_MODELS_EQUITY_LAST_STEP_BLACK_HPP
#define QM_MODELS_EQUITY_LAST_STEP_BLACK_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/sample.hpp"

/**
 * @file last_step_black.hpp
 * @brief lastStepCall / lastStepPut / lastStepDigital with a strike the
 *        script computes (issue #363).
 *
 * A literal strike is handed to the model, which computes the expectation
 * over its last step into the date (last_step.hpp). A strike the script
 * computes -- `lastStepDigital(strike * s0)`, s0 a fixing of the path -- is
 * only known once the path reaches the date, so the model writes the step's
 * forward F and log-variance v instead (StepQuantity::LastStepForward and
 * LastStepVariance), and the payoff applies Black's formula with its K.
 *
 * The expression is the IR's (scripting/ir_path.cpp, last_step_expectation),
 * so that the interpreters, the tape and the compiled kernels agree:
 * sd = sqrt(max(v, 1e-200)), N(x) = erfc(-x / sqrt 2) / 2 on doubles (the
 * IR's Ncdf), each AD type's own normal_cdf otherwise. A step with no
 * variance then gives the payoff of F, as last_step_expectation does, except
 * exactly at the strike.
 */

namespace quantModeling::mc
{
    namespace last_step_black_detail
    {
        QM_HOST_DEVICE inline double ncdf(double x)
        {
            return 0.5 * erfc(-x * 0.70710678118654752440);
        }
        template <class T>
        T ncdf(const T &x)
        {
            return T(normal_cdf(x)); // the AD type's, found by ADL
        }
    } // namespace last_step_black_detail

    template <class T>
    QM_HOST_DEVICE T last_step_black(StepQuantity kind, const T &forward, const T &var, const T &strike)
    {
        using last_step_black_detail::ncdf;
        using std::log;
        using std::sqrt;
        const T sd = sqrt(var > 1e-200 ? var : T(1e-200));
        const T d1 = log(forward / strike) / sd + 0.5 * sd;
        const T d2 = d1 - sd;
        switch (kind)
        {
            case StepQuantity::LastStepCall:
                return forward * ncdf(d1) - strike * ncdf(d2);
            case StepQuantity::LastStepPut:
                return strike * ncdf(T(-d2)) - forward * ncdf(T(-d1));
            default:
                return ncdf(d2);
        }
    }
} // namespace quantModeling::mc

#endif // QM_MODELS_EQUITY_LAST_STEP_BLACK_HPP
