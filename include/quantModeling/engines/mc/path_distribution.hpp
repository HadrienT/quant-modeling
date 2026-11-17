#ifndef ENGINE_MC_PATH_DISTRIBUTION_HPP
#define ENGINE_MC_PATH_DISTRIBUTION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quantModeling
{

    /**
     * What the simulation page measures on a model (blueprint/wp/29-simulation-diagnostics.md):
     * the law of the spot over time, its instantaneous volatility, and the
     * prices the paths imply -- the vanilla smile, the forward smile and a
     * path-dependent option -- each with its Monte Carlo standard error.
     *
     * The paths are those of the pricing models themselves (the timeline
     * models of models/equity/, on their own internal steps), not of a
     * display-only scheme: what is measured here is what the script pricer
     * would price with.
     */
    struct DistributionSettings
    {
        std::size_t n_steps = 100; ///< dates on a regular grid to ttm
        long long n_paths = 10000; ///< paths of the estimators
        long long n_display = 30;  ///< the first paths, returned whole
        std::uint64_t seed = 1;
        std::vector<Real> quantiles{0.05, 0.25, 0.5, 0.75, 0.95};
        std::size_t n_bins = 50; ///< histogram of S_t at each date

        /// Vanilla smiles at these dates (indices into the time grid, >= 1),
        /// strikes K = m F(0, t) for each m of smile_moneyness.
        std::vector<std::size_t> smile_dates;
        std::vector<Real> smile_moneyness;

        /// Forward smile from date index forward_start (>= 1, < n_steps) to
        /// the last date: options on S_T / S_t1 struck at each m of
        /// smile_moneyness times the forward ratio. 0 = none.
        std::size_t forward_start = 0;

        /// Up-and-out call on the last date, the barrier checked on every
        /// date of the grid, both levels as multiples of the spot. 0 = none.
        Real barrier = 0.0;
        Real barrier_strike = 0.0;
    };

    /// Prices on one strike grid, with the implied vols they invert to.
    struct SmileEstimate
    {
        std::size_t date = 0; ///< index into the time grid (the start date for a forward smile)
        Real ttm = 0.0;       ///< the options' maturity (T - t1 for a forward smile)
        Real forward = 0.0;   ///< F(0, t), or the forward ratio e^{(r - q)(T - t1)}
        std::vector<Real> moneyness;
        std::vector<Real> strikes;
        std::vector<Real> prices;       ///< out of the money (put below the forward), discounted to today
        std::vector<Real> price_stderr; ///< Monte Carlo standard error of each price
        std::vector<Real> implied_vols; ///< Black-76; NaN outside the no-arbitrage bounds
        std::vector<Real> iv_stderr;    ///< price_stderr / vega
    };

    struct PathDistribution
    {
        std::vector<Real> time_grid; ///< size n_steps + 1, time_grid[0] = 0
        long long n_paths = 0;

        std::vector<std::vector<Real>> paths; ///< [p][i], the first n_display paths
        std::vector<std::vector<Real>> vols;  ///< [p][i], their instantaneous vols (NaN at i = 0)

        std::vector<Real> quantile_levels;
        std::vector<std::vector<Real>> spot_quantiles; ///< [q][i]
        std::vector<std::vector<Real>> vol_quantiles;  ///< [q][i], NaN at i = 0; empty without has_vol
        bool has_vol = false;                          ///< the model fills Sample::inst_vol

        /// The martingale check: E[S_t / N_t] against its exact value S0 e^{-q t}.
        std::vector<Real> deflated_mean;
        std::vector<Real> deflated_stderr;
        std::vector<Real> deflated_exact;

        /// At-the-money-forward implied vol at each date (NaN at i = 0).
        std::vector<Real> atm_vol;
        std::vector<Real> atm_vol_stderr;

        /// Histogram of S_t at each date: n_bins equal bins on [lo, hi] (the
        /// 0.5% and 99.5% quantiles), counts of paths inside each.
        std::vector<Real> hist_lo, hist_hi;
        std::vector<std::vector<long long>> hist_counts; ///< [i][b]; empty bins at i = 0

        std::vector<SmileEstimate> smiles;
        bool has_forward_smile = false;
        SmileEstimate forward_smile;

        bool has_barrier = false;
        Real barrier_price = 0.0, barrier_stderr = 0.0;
        Real vanilla_price = 0.0, vanilla_stderr = 0.0; ///< the same call without the barrier, same paths
    };

    /**
     * Simulates `settings.n_paths` paths of `model` on a regular grid of
     * n_steps dates to ttm (the model takes its own steps in between) and
     * measures them. `spot`, `r` and `q` are the model's: they give the
     * forwards F(0, t) = spot e^{(r - q) t} the strikes are set on, and the
     * exact deflated mean; payoffs are discounted with the paths' own
     * numeraire. The gaussians are drawn exactly as simulate_model_paths
     * draws them, so the display paths are the paths it returns for the
     * same seed.
     */
    PathDistribution simulate_path_distribution(
        ISimulationModel<Real> &model, Real spot, Real r, Real q, Real ttm,
        const DistributionSettings &settings = {});

} // namespace quantModeling

#endif
