#include "quantModeling/risk/exposure_metrics.hpp"

#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{
    namespace
    {
        void require_normal_parameters(Real mu, Real sigma)
        {
            if (!std::isfinite(mu) || !std::isfinite(sigma) || sigma < 0.0)
                throw InvalidInput("normal exposure: mu must be finite and sigma finite and >= 0");
        }

        void require_profile(const std::vector<Time> &times, const std::vector<Real> &values)
        {
            if (times.empty() || times.size() != values.size())
                throw InvalidInput("exposure profile requires matching non-empty times and values");
            Time previous = -1.0;
            for (std::size_t i = 0; i < times.size(); ++i)
            {
                if (!(times[i] >= 0.0) || !(times[i] > previous))
                    throw InvalidInput("exposure profile times must be >= 0 and strictly increasing");
                previous = times[i];
            }
            if (!(times.back() > 0.0))
                throw InvalidInput("exposure profile must extend past t = 0");
        }
    } // namespace

    Real normal_expected_exposure(Real mu, Real sigma)
    {
        require_normal_parameters(mu, sigma);
        if (sigma == 0.0)
            return std::max(mu, 0.0);
        const Real d = mu / sigma;
        return mu * norm_cdf(d) + sigma * norm_pdf(d);
    }

    Real normal_expected_negative_exposure(Real mu, Real sigma)
    {
        require_normal_parameters(mu, sigma);
        if (sigma == 0.0)
            return std::min(mu, 0.0);
        const Real d = mu / sigma;
        return mu * norm_cdf(-d) - sigma * norm_pdf(d);
    }

    Real normal_potential_future_exposure(Real mu, Real sigma, Real confidence)
    {
        require_normal_parameters(mu, sigma);
        if (!(confidence > 0.0 && confidence < 1.0))
            throw InvalidInput("PFE confidence level must be in (0, 1)");
        return mu + sigma * inverse_normal_cdf(confidence);
    }

    Real netting_factor(std::size_t n, Real average_correlation)
    {
        if (n == 0)
            throw InvalidInput("netting factor requires at least one trade");
        const Real count = static_cast<Real>(n);
        // Variance of the sum of n unit-variance exposures, in units of one.
        const Real variance = count + count * (count - 1.0) * average_correlation;
        if (!(average_correlation <= 1.0) || variance < -1e-12)
            throw InvalidInput("netting factor: average correlation must be in [-1/(n-1), 1]");
        return std::sqrt(std::max(variance, 0.0)) / count;
    }

    Real expected_positive_exposure(const std::vector<Time> &times, const std::vector<Real> &ee)
    {
        require_profile(times, ee);
        Real integral = 0.0;
        Time previous = 0.0;
        for (std::size_t i = 0; i < times.size(); ++i)
        {
            integral += ee[i] * (times[i] - previous);
            previous = times[i];
        }
        return integral / times.back();
    }

    std::vector<Real> effective_expected_exposure(const std::vector<Real> &ee)
    {
        std::vector<Real> effective(ee.size());
        Real running = 0.0;
        for (std::size_t i = 0; i < ee.size(); ++i)
        {
            running = i == 0 ? ee[i] : std::max(running, ee[i]);
            effective[i] = running;
        }
        return effective;
    }

    Real effective_expected_positive_exposure(const std::vector<Time> &times,
                                              const std::vector<Real> &ee, Time horizon)
    {
        require_profile(times, ee);
        if (!(horizon > 0.0))
            throw InvalidInput("EEPE horizon must be > 0");
        const std::vector<Real> effective = effective_expected_exposure(ee);
        const Time end = std::min(horizon, times.back());
        Real integral = 0.0;
        Time previous = 0.0;
        for (std::size_t i = 0; i < times.size() && previous < end; ++i)
        {
            // EEE(t_i) covers (t_{i-1}, t_i], cut at the horizon.
            const Time upper = std::min(times[i], end);
            integral += effective[i] * (upper - previous);
            previous = upper;
        }
        return integral / end;
    }

} // namespace quantModeling
