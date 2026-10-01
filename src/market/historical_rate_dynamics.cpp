#include "quantModeling/market/historical_rate_dynamics.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr std::size_t kMinObservations = 30;

        void require_series(const std::vector<Real> &rates, Time dt)
        {
            if (rates.size() < kMinObservations)
                throw InvalidInput("historical rate dynamics: at least 30 observations are required");
            if (!(dt > 0.0) || !std::isfinite(dt))
                throw InvalidInput("historical rate dynamics: the time step must be > 0");
            for (const Real r : rates)
                if (!std::isfinite(r))
                    throw InvalidInput("historical rate dynamics: the series holds a non-finite value");
        }
    } // namespace

    Time HistoricalRateDynamics::half_life() const
    {
        return std::log(2.0) / mean_reversion;
    }

    void HistoricalRateDynamics::validate() const
    {
        if (!(mean_reversion > 0.0) || !std::isfinite(mean_reversion))
            throw InvalidInput("historical rate dynamics: the mean reversion must be > 0");
        if (!(sigma > 0.0) || !std::isfinite(sigma))
            throw InvalidInput("historical rate dynamics: sigma must be > 0");
        if (!std::isfinite(long_run_rate))
            throw InvalidInput("historical rate dynamics: the long-run rate must be finite");
    }

    HistoricalRateEstimate estimate_historical_rate_dynamics(const std::vector<Real> &rates, Time dt)
    {
        require_series(rates, dt);
        const std::size_t n = rates.size() - 1; // pairs (r_k, r_{k+1})
        const Real count = static_cast<Real>(n);

        Real mean_x = 0.0, mean_y = 0.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            mean_x += rates[k];
            mean_y += rates[k + 1];
        }
        mean_x /= count;
        mean_y /= count;
        Real sxx = 0.0, sxy = 0.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            sxx += (rates[k] - mean_x) * (rates[k] - mean_x);
            sxy += (rates[k] - mean_x) * (rates[k + 1] - mean_y);
        }
        if (!(sxx > 0.0))
            throw InvalidInput("historical rate dynamics: the series is constant");

        const Real b = sxy / sxx;
        if (!(b > 0.0 && b < 1.0))
            throw InvalidInput(
                "historical rate dynamics: no measurable mean reversion in this sample (the rate "
                "behaves like a random walk over it); use a longer history or fix the mean reversion");
        const Real intercept = mean_y - b * mean_x;
        Real residual = 0.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            const Real e = rates[k + 1] - intercept - b * rates[k];
            residual += e * e;
        }
        const Real residual_variance = residual / (count - 2.0);

        HistoricalRateEstimate out;
        out.observations = rates.size();
        const Real a = -std::log(b) / dt;
        out.dynamics.mean_reversion = a;
        out.dynamics.long_run_rate = intercept / (1.0 - b);
        out.dynamics.sigma = std::sqrt(residual_variance * 2.0 * a / (1.0 - b * b));

        // Standard errors: of the regression slope, carried to a = -ln(b)/dt
        // (delta method); of the mean of an AR(1), whose observations are not
        // independent — the long-run variance is Var ε / (1 - b)²; of a
        // standard deviation estimated on n residuals.
        const Real b_error = std::sqrt(residual_variance / sxx);
        out.mean_reversion_std_error = b_error / (b * dt);
        out.long_run_rate_std_error = std::sqrt(residual_variance / count) / (1.0 - b);
        out.sigma_std_error = out.dynamics.sigma / std::sqrt(2.0 * count);
        return out;
    }

    Real estimate_historical_volatility(const std::vector<Real> &rates, Time dt)
    {
        require_series(rates, dt);
        const std::size_t n = rates.size() - 1;
        Real mean = 0.0;
        for (std::size_t k = 0; k < n; ++k)
            mean += rates[k + 1] - rates[k];
        mean /= static_cast<Real>(n);
        Real sum = 0.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            const Real d = rates[k + 1] - rates[k] - mean;
            sum += d * d;
        }
        const Real variance = sum / static_cast<Real>(n - 1);
        if (!(variance > 0.0))
            throw InvalidInput("historical rate dynamics: the series is constant");
        return std::sqrt(variance / dt);
    }

} // namespace quantModeling
