#ifndef ENGINE_MC_KERNELS_VANILLA_BS_HPP
#define ENGINE_MC_KERNELS_VANILLA_BS_HPP

#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/gaussian_source.hpp"

/**
 * @file vanilla_bs.hpp
 * @brief Templated Monte-Carlo path kernel for European vanillas under
 *        flat Black-Scholes (terminal sampling, no time-stepping).
 *
 * Design (foundation for QMC / AAD / CUDA phases):
 *  - The kernel is a free function templated on
 *      * OptionType   (call/put resolved at compile time — no branch per path),
 *      * Antithetic   (compile-time bool — no mode check per path),
 *      * Source       (GaussianSource concept — draw fully inlined).
 *    One virtual call per *request* remains (the visitor), zero per path.
 *  - All market/contract constants are precomputed once into a plain
 *    aggregate (VanillaTerminalSpec) → the same kernel is compilable by
 *    nvcc later (QM_HOST_DEVICE, no std::shared_ptr, no virtual calls).
 *  - Estimators per path: payoff, pathwise delta, LRM vega/rho,
 *    FD gamma/theta with common random numbers (same math as the
 *    original engine, now written once).
 */

namespace quantModeling::mc
{

    /// Precomputed constants for terminal GBM sampling and CRN greek bumps.
    struct VanillaTerminalSpec
    {
        Real S0 = 0.0;
        Real K = 0.0;
        Real sigma = 0.0;
        Real T = 0.0;
        Real sqrtT = 0.0;

        // Terminal sampling: ST = movedSpot * exp(rootVariance * z)
        Real movedSpot = 0.0;    // S0 * exp((r - q - sigma^2/2) T)
        Real rootVariance = 0.0; // sigma * sqrt(T)
        Real df = 0.0;           // discount(T)

        // Spot bumps for FD gamma (common random numbers)
        Real dS = 0.0;
        Real factor_up = 0.0; // (S0 + dS) / S0
        Real factor_dn = 0.0; // (S0 - dS) / S0

        // Time bumps for FD theta (common random numbers)
        Real theta_bump = 0.0;
        Real movedSpot_upT = 0.0;
        Real movedSpot_dnT = 0.0;
        Real rootVariance_upT = 0.0;
        Real rootVariance_dnT = 0.0;
        Real df_upT = 0.0;
        Real df_dnT = 0.0;
    };

    /// Per-path estimator values (all linear ⇒ antithetic = plain average).
    struct VanillaPathValues
    {
        Real payoff = 0.0;
        Real delta = 0.0;
        Real vega = 0.0;
        Real rho = 0.0;
        Real gamma = 0.0;
        Real theta = 0.0;
    };

    /// Accumulated statistics for one simulation.
    struct alignas(QM_CACHELINE) VanillaStats
    {
        WelfordAccumulator payoff;
        WelfordAccumulator delta;
        WelfordAccumulator vega;
        WelfordAccumulator rho;
        WelfordAccumulator gamma;
        WelfordAccumulator theta;

        QM_HOST_DEVICE void add(const VanillaPathValues &v)
        {
            payoff.add(v.payoff);
            delta.add(v.delta);
            vega.add(v.vega);
            rho.add(v.rho);
            gamma.add(v.gamma);
            theta.add(v.theta);
        }
    };

    template <OptionType CP>
    QM_HOST_DEVICE inline Real vanilla_payoff(Real S, Real K)
    {
        if constexpr (CP == OptionType::Call)
            return (S > K) ? (S - K) : Real(0);
        else
            return (K > S) ? (K - S) : Real(0);
    }

    /// Evaluate every estimator for a single Gaussian draw z.
    template <OptionType CP>
    QM_HOST_DEVICE inline VanillaPathValues
    eval_vanilla_path(const VanillaTerminalSpec &s, Real z)
    {
        VanillaPathValues out;

        const Real ST = s.movedSpot * std::exp(s.rootVariance * z);
        out.payoff = vanilla_payoff<CP>(ST, s.K);

        // Pathwise delta: d(payoff)/dST × dST/dS0 × discount
        if constexpr (CP == OptionType::Call)
            out.delta = (ST > s.K) ? s.df * (ST / s.S0) : Real(0);
        else
            out.delta = (ST < s.K) ? -s.df * (ST / s.S0) : Real(0);

        // Likelihood-ratio scores
        const Real score_sigma = (z * z - 1.0) / s.sigma;
        const Real score_r = (z * s.sqrtT) / s.sigma;
        out.vega = out.payoff * score_sigma;
        out.rho = -s.T * out.payoff + out.payoff * score_r;

        // FD gamma with common random numbers (spot bumps)
        const Real payoff_up = vanilla_payoff<CP>(ST * s.factor_up, s.K);
        const Real payoff_dn = vanilla_payoff<CP>(ST * s.factor_dn, s.K);
        out.gamma = s.df * (payoff_up - 2.0 * out.payoff + payoff_dn) / (s.dS * s.dS);

        // FD theta with common random numbers (time bumps)
        const Real ST_Tup = s.movedSpot_upT * std::exp(s.rootVariance_upT * z);
        const Real ST_Tdn = s.movedSpot_dnT * std::exp(s.rootVariance_dnT * z);
        const Real payoff_Tup = vanilla_payoff<CP>(ST_Tup, s.K);
        const Real payoff_Tdn = vanilla_payoff<CP>(ST_Tdn, s.K);
        out.theta = (s.df_dnT * payoff_Tdn - s.df_upT * payoff_Tup) / (2.0 * s.theta_bump);

        return out;
    }

    QM_HOST_DEVICE inline VanillaPathValues
    average_pair(const VanillaPathValues &a, const VanillaPathValues &b)
    {
        VanillaPathValues out;
        out.payoff = 0.5 * (a.payoff + b.payoff);
        out.delta = 0.5 * (a.delta + b.delta);
        out.vega = 0.5 * (a.vega + b.vega);
        out.rho = 0.5 * (a.rho + b.rho);
        out.gamma = 0.5 * (a.gamma + b.gamma);
        out.theta = 0.5 * (a.theta + b.theta);
        return out;
    }

    /**
     * @brief Run the full simulation.
     *
     * With Antithetic = true, paths are consumed as (z, -z) pairs whose
     * estimator values are averaged before accumulation (one Welford sample
     * per pair); a trailing odd path is processed alone.
     */
    template <OptionType CP, bool Antithetic, GaussianSource Source>
    VanillaStats simulate_vanilla_terminal(const VanillaTerminalSpec &spec,
                                           int n_paths, Source &gauss)
    {
        VanillaStats stats;

        if constexpr (Antithetic)
        {
            const int n_pairs = n_paths / 2;
            const bool has_odd = (n_paths % 2) != 0;

            for (int i = 0; i < n_pairs; ++i)
            {
                const Real z = gauss.next();
                const VanillaPathValues p = eval_vanilla_path<CP>(spec, z);
                const VanillaPathValues m = eval_vanilla_path<CP>(spec, -z);
                stats.add(average_pair(p, m));
            }
            if (has_odd)
            {
                const Real z = gauss.next();
                stats.add(eval_vanilla_path<CP>(spec, z));
            }
        }
        else
        {
            for (int i = 0; i < n_paths; ++i)
            {
                const Real z = gauss.next();
                stats.add(eval_vanilla_path<CP>(spec, z));
            }
        }

        return stats;
    }

} // namespace quantModeling::mc

#endif // ENGINE_MC_KERNELS_VANILLA_BS_HPP
