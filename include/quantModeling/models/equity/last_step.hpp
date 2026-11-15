#ifndef QM_MODELS_EQUITY_LAST_STEP_HPP
#define QM_MODELS_EQUITY_LAST_STEP_HPP

#include <cmath>
#include <vector>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/utils/stats.hpp"

/**
 * @file last_step.hpp
 * @brief The expectation of a vanilla payoff over a model's last step
 *        (lastStepCall / lastStepPut / lastStepDigital, WP 27 §8 rung 1).
 *
 * Under a log-Euler step S' = S exp(m + sqrt(v) z) -- Black-Scholes's exact
 * transition, and local vol's step with the vol frozen at its start -- the
 * spot at the end of the step is lognormal given the path before it, with
 * forward F = S exp(m + v / 2) and log-variance v. A payoff of S' alone then
 * has Black's formula as its conditional expectation:
 *
 *     E[max(S' - K, 0)] = F N(d1) - K N(d2)
 *     E[max(K - S', 0)] = K N(-d2) - F N(-d1)
 *     P(S' > K)         = N(d2),       d1 = ln(F / K) / sqrt(v) + sqrt(v) / 2,
 *                                      d2 = d1 - sqrt(v).
 *
 * The scheme's own law, so the price has no bias against the payoff it
 * replaces (the tower property), and the result is smooth to every order in
 * S and in the step's coefficients: a path's second derivative is the true
 * one, where the payoff's kink would have given 0 (WP 27 §8). A step with no
 * variance (v = 0) gives the payoff of F.
 *
 * Generic in the number type: double, the tape's Number and the second
 * order's Tangent<Number>; N(x) is each type's own normal_cdf.
 */

namespace quantModeling::mc
{

    namespace last_step_detail
    {
        inline double normal_cdf_of(double x)
        {
            return norm_cdf(x);
        }
        /// The AD types' normal_cdf (aad::normal_cdf, found by ADL).
        template <class T>
        T normal_cdf_of(const T &x)
        {
            return T(normal_cdf(x));
        }
    } // namespace last_step_detail

    /// E[payoff(S')] over the step, `forward` = E[S'], `var` = the step's
    /// log-variance, `strike` = K; `kind` is one of the LastStep* quantities.
    template <class T>
    T last_step_expectation(StepQuantity kind, const T &forward, const T &var, double strike)
    {
        using last_step_detail::normal_cdf_of;
        using std::log;
        using std::sqrt;
        if (!(var > 0.0))
        {
            // No variance: the payoff of the forward itself.
            switch (kind)
            {
                case StepQuantity::LastStepCall:
                    return forward > strike ? T(forward - strike) : T(0.0);
                case StepQuantity::LastStepPut:
                    return forward < strike ? T(strike - forward) : T(0.0);
                default:
                    return forward > strike ? T(1.0) : T(0.0);
            }
        }
        const T sd = sqrt(var);
        const T d1 = log(forward / strike) / sd + 0.5 * sd;
        const T d2 = d1 - sd;
        switch (kind)
        {
            case StepQuantity::LastStepCall:
                return forward * normal_cdf_of(d1) - strike * normal_cdf_of(d2);
            case StepQuantity::LastStepPut:
                return strike * normal_cdf_of(T(-d2)) - forward * normal_cdf_of(T(-d1));
            default:
                return normal_cdf_of(d2);
        }
    }

    /// Whether the defline asks for a last-step expectation.
    inline bool asks_last_step(const std::vector<SampleDef> &defline)
    {
        for (const SampleDef &def : defline)
            for (const BarrierMonitor &b : def.barriers)
                if (b.kind != StepQuantity::Survival)
                    return true;
        return false;
    }

    /// Writes every last-step slot of an event from the step that reached
    /// it: forward F = E[S'] and log-variance v (bridge survival slots are
    /// left alone).
    template <class T>
    void fill_last_step(const SampleDef &def, Sample<T> &smp, const T &forward, const T &var)
    {
        smp.survivals.resize(def.barriers.size());
        for (std::size_t b = 0; b < def.barriers.size(); ++b)
            if (def.barriers[b].kind != StepQuantity::Survival)
                smp.survivals[b] = last_step_expectation(def.barriers[b].kind, forward, var, def.barriers[b].level);
    }

} // namespace quantModeling::mc

#endif // QM_MODELS_EQUITY_LAST_STEP_HPP
