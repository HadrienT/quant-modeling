#ifndef EQUITY_SABR_PDE_HPP
#define EQUITY_SABR_PDE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/sabr.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * Arbitrage-free SABR (Hagan, Kumar, Lesniewski, Woodward, "Arbitrage-Free
     * SABR", Wilmott 2014): sabr_implied_vol()'s closed-form asymptotic
     * expansion can price options that are not consistent with any
     * risk-neutral density (negative density at low strikes, worse for beta
     * away from 1, large nu, or long maturity). This solves the equivalent
     * Dupire-style forward PDE for the call price directly instead:
     *
     *   dC/dT(T,K) = (1/2) D(K)^2 E(T,K) d^2C/dK^2(T,K),   C(0,K) = (f-K)^+
     *
     *   Gamma(K) = (K^beta - f^beta) / (K - f)        [-> beta*f^(beta-1) at K=f]
     *   y(K)     = (K^(1-beta) - f^(1-beta)) / (1-beta)
     *   D(K)     = sqrt(alpha^2 + 2*alpha*rho*nu*y(K) + nu^2*y(K)^2) * K^beta
     *   E(T,K)   = exp(rho*nu*alpha*Gamma(K)*T)
     *
     * A price built this way is a genuine expectation under a real diffusion,
     * so it is automatically free of butterfly and calendar arbitrage --
     * unlike the closed form, there is nothing to check afterward.
     *
     * Deviation from the paper, recorded here rather than left silent: Hagan
     * et al. solve the equivalent Fokker-Planck density PDE with a flux
     * (moment-preserving) boundary condition that exactly conserves
     * probability mass and the forward. This solves the Dupire price PDE
     * above with the simpler linear (zero second-derivative) boundary
     * condition at both ends instead -- the same simplification
     * Andreasen & Huge's ZABR uses, and the one Le Floc'h & Kennedy ("Finite
     * difference techniques for arbitrage-free SABR", 2015) present as a
     * legitimate lighter-weight alternative. Ghost-point substitution of
     * "zero second derivative" into the interior discretisation makes the
     * diffusion term vanish exactly at the boundary nodes, so in this scheme
     * that condition is equivalent to holding C fixed at its T=0 value at
     * both edges of the grid -- reasonable as long as the grid extends far
     * enough into each tail that the option is already close to its
     * boundary limit there (intrinsic value on the left, zero on the right),
     * which the strike-range heuristic below exists to guarantee. No
     * Lamperti transform (accuracy refinement, not a correctness fix) in
     * this first pass.
     *
     * Numerical scheme: Crank-Nicolson with Rannacher start-up (the first
     * settings.n_rannacher_steps steps run fully implicit instead), the
     * standard fix (etc/roadmap.md already names it) for the oscillations
     * Crank-Nicolson produces on the non-smooth (kinked) terminal condition.
     */
    struct SABRPDESettings
    {
        std::size_t n_space = 200;         ///< strike grid points
        std::size_t n_time = 100;          ///< time steps from 0 to ttm
        std::size_t n_rannacher_steps = 4; ///< fully-implicit warm-up steps
        Real grid_std_devs = 6.0;          ///< grid half-width, in local-vol-scale standard deviations
    };

    struct SABRPDEResult
    {
        std::vector<Real> strikes;     ///< == the caller's requested strikes, in the order given
        std::vector<Real> call_prices; ///< undiscounted: E[(F_ttm - K)^+], not multiplied by any discount factor
    };

    /// Solves the PDE from T=0 to T=ttm and interpolates the resulting price
    /// curve onto `strikes`. One solve prices every requested strike at once
    /// (they share the same PDE grid), so a calibration slice with many
    /// strikes needs only one solve per residual evaluation, not one per
    /// strike.
    SABRPDEResult sabr_arbitrage_free_prices(
        Real forward, Real ttm, const SABRParams &params,
        const std::vector<Real> &strikes,
        const SABRPDESettings &settings = {});

    /// Convenience: sabr_arbitrage_free_prices() followed by
    /// black76_implied_vol() per strike, applying discount_factor to the
    /// PDE's undiscounted prices first. NaN at any strike where the
    /// resulting (discounted) price falls outside Black-76's no-arbitrage
    /// bounds -- see black76_implied_vol.
    std::vector<Real> sabr_arbitrage_free_implied_vols(
        Real forward, Real ttm, const SABRParams &params,
        const std::vector<Real> &strikes, Real discount_factor,
        const SABRPDESettings &settings = {});

} // namespace quantModeling

#endif
