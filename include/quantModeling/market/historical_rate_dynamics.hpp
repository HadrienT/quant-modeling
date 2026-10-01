#ifndef QM_MARKET_HISTORICAL_RATE_DYNAMICS_HPP
#define QM_MARKET_HISTORICAL_RATE_DYNAMICS_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief How the short rate has actually moved: its dynamics under the
     *        historical (real-world) measure, as a mean-reverting Gaussian
     *        process (blueprint/wp/23-xva.md §3.1, lot X1b)
     *
     *   dr = a (θ - r) dt + σ dW.
     *
     * Gregory's distinction: CVA is a price, so its exposure is simulated
     * under the risk-neutral measure, calibrated to option prices. PFE and
     * credit limits are risk measures — "how large can the exposure really
     * get" — and belong to the historical measure: the volatility rates have
     * shown, around the level they have reverted to. The two differ: implied
     * volatility carries a risk premium, and the forward curve is not a
     * forecast.
     *
     * The exposure engine simulates the rate with these dynamics and still
     * **prices** each trade, on each path, with the risk-neutral model: what
     * a swap would be worth in a scenario is a market price, whatever the
     * probability of the scenario.
     */
    struct HistoricalRateDynamics
    {
        /// a: speed of mean reversion, per year.
        Real mean_reversion = 0.0;
        /// θ: the level the rate reverts to (a decimal: 0.03 = 3 %).
        Real long_run_rate = 0.0;
        /// σ: absolute volatility of the rate, per sqrt(year).
        Real sigma = 0.0;

        /// Time for a deviation from θ to halve: ln 2 / a.
        Time half_life() const;
        /// @throws InvalidInput unless a > 0, σ > 0 and θ is finite.
        void validate() const;
    };

    /// An estimate, with what it rests on. The standard errors matter: the
    /// volatility is well determined by a few years of daily data, the mean
    /// reversion and the long-run level are not (a rate that takes years to
    /// revert has reverted only a few times in the sample).
    struct HistoricalRateEstimate
    {
        HistoricalRateDynamics dynamics;
        Real mean_reversion_std_error = 0.0;
        Real long_run_rate_std_error = 0.0;
        Real sigma_std_error = 0.0;
        std::size_t observations = 0;
    };

    /**
     * @brief Estimates (a, θ, σ) from a series of the rate observed every
     *        `dt` years, by maximum likelihood.
     *
     * The process sampled at a fixed step is exactly an AR(1),
     *
     *   r_{k+1} = θ (1 - b) + b r_k + ε,  b = e^{-a dt},  Var ε = σ² (1 - b²) / (2a),
     *
     * so the maximum-likelihood estimates are the least-squares regression of
     * r_{k+1} on r_k, mapped back (no Euler discretisation error).
     *
     * @param rates the observations, oldest first, as decimals.
     * @param dt    their spacing in years (1/252 for business days).
     * @throws InvalidInput on fewer than 30 observations, a constant series,
     *         or a series with no measurable mean reversion (b >= 1: it
     *         behaves like a random walk over the sample — use a longer
     *         history, or estimate_historical_volatility with a chosen a).
     */
    HistoricalRateEstimate estimate_historical_rate_dynamics(const std::vector<Real> &rates, Time dt);

    /**
     * @brief The volatility alone, sd(Δr) / sqrt(dt), for when the mean
     *        reversion and the long-run level are chosen rather than
     *        estimated. Over a daily step the drift is negligible against
     *        the diffusion, so this does not depend on them.
     */
    Real estimate_historical_volatility(const std::vector<Real> &rates, Time dt);

} // namespace quantModeling

#endif // QM_MARKET_HISTORICAL_RATE_DYNAMICS_HPP
