#ifndef ENGINE_MC_PATH_SIMULATION_HPP
#define ENGINE_MC_PATH_SIMULATION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/sabr.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quantModeling
{

    struct PathSimulationSettings
    {
        std::size_t n_steps = 100;
        long long n_paths = 30; ///< a display chart, not a pricing estimator -- a few dozen paths is plenty to look at
        std::uint64_t seed = 1;
    };

    struct SimulatedPaths
    {
        std::vector<Real> time_grid;          ///< size n_steps + 1, time_grid[0] = 0
        std::vector<std::vector<Real>> paths; ///< paths[p][i] = underlying at time_grid[i] on path p
    };

    /**
     * Exact GBM paths under the risk-neutral (or T-forward, with q=0) measure:
     * S_{t+dt} = S_t * exp((r - q - 0.5*sigma^2)*dt + sigma*sqrt(dt)*Z). No
     * discretisation error -- this is the exact transition density of Black-
     * Scholes, so there is nothing to verify beyond the standard GBM
     * closed-form solution.
     */
    SimulatedPaths simulate_black_scholes_paths(
        Real spot, Real r, Real q, Real sigma, Real ttm,
        const PathSimulationSettings &settings = {});

    /**
     * Illustrative SABR paths via Euler discretisation of
     *   dF = alpha*F^beta*dW1, dalpha = nu*alpha*dW2, dW1 dW2 = rho dt.
     *
     * alpha's own marginal SDE (dalpha = nu*alpha*dW2, no drift) is a
     * driftless GBM and is simulated exactly:
     * alpha_{t+dt} = alpha_t * exp(-0.5*nu^2*dt + nu*sqrt(dt)*Z2). F has no
     * such closed form for beta != 0, 1 (that is precisely why SABR pricing
     * goes through Hagan's formula or the arbitrage-free PDE rather than
     * simulation -- see models/equity/sabr_pde.hpp), so F is advanced by a
     * plain Euler step, absorbed at 0 if it would go negative (F^beta is
     * undefined below 0 for non-integer beta).
     *
     * This is a display/illustration tool, not a pricing engine: it is not
     * held to the same discretisation-bias standard as
     * models/equity/sabr_pde.hpp's arbitrage-free solver, and should not be
     * used to price anything.
     */
    SimulatedPaths simulate_sabr_paths(
        Real forward, const SABRParams &params, Real ttm,
        const PathSimulationSettings &settings = {});

} // namespace quantModeling

#endif
