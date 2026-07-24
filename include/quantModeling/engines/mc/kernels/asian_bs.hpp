#ifndef ENGINE_MC_KERNELS_ASIAN_BS_HPP
#define ENGINE_MC_KERNELS_ASIAN_BS_HPP

#include <cmath>
#include <span>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/variance_reduction/control_variate.hpp"

/**
 * @file asian_bs.hpp
 * @brief QMC Monte-Carlo kernel for discretely monitored Asian options
 *        under flat Black-Scholes.
 *
 * Techniques combined per path (the ROADMAP flagship case):
 *  - Sobol point per path (dimension = number of fixings), scrambled;
 *  - Brownian-bridge construction: the first (best-distributed) Sobol
 *    coordinates carry the terminal value and coarse path shape —
 *    reduces effective dimension, restoring near-O(1/N) QMC convergence;
 *  - geometric-Asian control variate: the discretely monitored geometric
 *    average is lognormal, so its price has an exact closed form
 *    (discrete Kemna-Vorst); the geometric payoff evaluated on the *same*
 *    path has correlation ≈0.99+ with the arithmetic payoff.
 *
 * Fixing grid: t_j = j·T/n, j = 1..n (matches BSEuroAsianMCEngine).
 */

namespace quantModeling::mc
{

    struct AsianSpec
    {
        Real S0 = 0.0;
        Real K = 0.0;
        Real r = 0.0;
        Real q = 0.0;
        Real sigma = 0.0;
        Real T = 0.0;
        int n_fixings = 0;
        Real df = 0.0; ///< discount factor to T
    };

    /**
     * @brief Exact price of the discretely monitored geometric Asian option.
     *
     * With fixings t_j = j·dt, log G = log S0 + (r−q−σ²/2)·t̄ + σ/n Σ W(t_j)
     * is Gaussian with
     *   t̄  = dt (n+1)/2
     *   Var = σ²/n² Σ_{j,k} min(t_j,t_k) = σ² dt (n+1)(2n+1)/(6n)
     * giving a Black-formula price on E[G] = exp(μ_G + σ_G²/2).
     */
    inline Real discrete_geometric_asian_price(const AsianSpec &s, OptionType type)
    {
        const Real n = static_cast<Real>(s.n_fixings);
        const Real dt = s.T / n;
        const Real t_bar = dt * (n + 1.0) / 2.0;
        const Real var_g = s.sigma * s.sigma * dt * (n + 1.0) * (2.0 * n + 1.0) / (6.0 * n);
        const Real sd_g = std::sqrt(var_g);

        const Real mu_g = std::log(s.S0) + (s.r - s.q - 0.5 * s.sigma * s.sigma) * t_bar;
        const Real fwd_g = std::exp(mu_g + 0.5 * var_g); // E[G]

        auto Phi = [](Real x)
        { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };

        const Real d1 = (mu_g + var_g - std::log(s.K)) / sd_g;
        const Real d2 = d1 - sd_g;

        if (type == OptionType::Call)
            return s.df * (fwd_g * Phi(d1) - s.K * Phi(d2));
        return s.df * (s.K * Phi(-d2) - fwd_g * Phi(-d1));
    }

    /// Reusable per-request buffers: no allocation inside the path loop.
    struct AsianWorkspace
    {
        BrownianBridge bridge;
        std::vector<Real> times; ///< fixing times t_1..t_n
        std::vector<Real> drift; ///< (r−q−σ²/2)·t_j precomputed
        std::vector<double> z;   ///< Sobol gaussian point
        std::vector<Real> w;     ///< Brownian path W(t_j)

        explicit AsianWorkspace(const AsianSpec &s)
            : bridge(make_times(s)), times(make_times(s)),
              drift(static_cast<size_t>(s.n_fixings)),
              z(static_cast<size_t>(s.n_fixings)),
              w(static_cast<size_t>(s.n_fixings))
        {
            const Real mu = s.r - s.q - 0.5 * s.sigma * s.sigma;
            for (int j = 0; j < s.n_fixings; ++j)
                drift[static_cast<size_t>(j)] = mu * times[static_cast<size_t>(j)];
        }

    private:
        static std::vector<Real> make_times(const AsianSpec &s)
        {
            std::vector<Real> t(static_cast<size_t>(s.n_fixings));
            const Real dt = s.T / static_cast<Real>(s.n_fixings);
            for (int j = 0; j < s.n_fixings; ++j)
                t[static_cast<size_t>(j)] = static_cast<Real>(j + 1) * dt;
            return t;
        }
    };

    /// Per-batch accumulated statistics.
    struct AsianBatchStats
    {
        ControlVariateAccumulator cv; ///< (Y = df·arith payoff, X = df·geo payoff)
        WelfordAccumulator delta;     ///< pathwise delta (exact: dA/dS0 = A/S0)
    };

    template <OptionType CP>
    QM_HOST_DEVICE inline Real asian_payoff(Real average, Real K)
    {
        if constexpr (CP == OptionType::Call)
            return (average > K) ? (average - K) : Real(0);
        else
            return (K > average) ? (K - average) : Real(0);
    }

    /**
     * @brief Simulate one RQMC batch of Asian paths.
     *
     * Each Sobol point (n_fixings coordinates) drives one path through the
     * Brownian bridge. Both the arithmetic and geometric averages are
     * computed on the same path; the target payoff Y uses the requested
     * average type AVG, the control X is always the geometric payoff
     * (free, and exactly priced by discrete_geometric_asian_price).
     */
    template <OptionType CP, AsianAverageType AVG>
    AsianBatchStats simulate_asian_batch(const AsianSpec &s, AsianWorkspace &ws,
                                         int n_paths, SobolSequence &seq)
    {
        AsianBatchStats stats;
        const Real inv_n = Real(1) / static_cast<Real>(s.n_fixings);

        for (int i = 0; i < n_paths; ++i)
        {
            seq.next_gaussian(ws.z);
            ws.bridge.transform(ws.z, ws.w);

            Real sum_S = 0.0;
            Real sum_log_S = 0.0;
            for (int j = 0; j < s.n_fixings; ++j)
            {
                const Real log_ratio = ws.drift[static_cast<size_t>(j)] +
                                       s.sigma * ws.w[static_cast<size_t>(j)];
                const Real S = s.S0 * std::exp(log_ratio);
                sum_S += S;
                sum_log_S += std::log(s.S0) + log_ratio;
            }

            const Real A = sum_S * inv_n;               // arithmetic average
            const Real G = std::exp(sum_log_S * inv_n); // geometric average
            const Real P = (AVG == AsianAverageType::Arithmetic) ? A : G;

            const Real y = s.df * asian_payoff<CP>(P, s.K);
            const Real x = s.df * asian_payoff<CP>(G, s.K);
            stats.cv.add(y, x);

            // Pathwise delta: every S_j scales linearly with S0 under flat BS,
            // hence dP/dS0 = P/S0 exactly (both average types).
            Real delta_val = 0.0;
            if constexpr (CP == OptionType::Call)
                delta_val = (P > s.K) ? s.df * (P / s.S0) : Real(0);
            else
                delta_val = (P < s.K) ? -s.df * (P / s.S0) : Real(0);
            stats.delta.add(delta_val);
        }
        return stats;
    }

} // namespace quantModeling::mc

#endif // ENGINE_MC_KERNELS_ASIAN_BS_HPP
