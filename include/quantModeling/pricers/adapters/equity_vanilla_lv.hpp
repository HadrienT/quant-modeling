#ifndef PRICERS_ADAPTERS_EQUITY_VANILLA_LV_HPP
#define PRICERS_ADAPTERS_EQUITY_VANILLA_LV_HPP

#include "quantModeling/core/results.hpp"
#include "quantModeling/pricers/inputs.hpp"

namespace quantModeling
{

    /**
     * @brief European vanilla under a Dupire local-vol surface, priced as a
     *        script by the generic engine (WP 26 P3,
     *        blueprint/wp/26-performance.md §7).
     *
     * The surface is the pre-evaluated K x T grid of the API's calibration
     * (LocalVolInput::sigma_loc_flat, K-major), interpolated as GridLocalVol.
     * The spot follows a log-Euler scheme with ceil(maturity *
     * n_steps_per_year) equal steps, the local vol read at each step's start.
     *
     * Sobol points (16 replicates, the error from their spread), on the GPU
     * when present: `mc_antithetic` is ignored. Greeks as every script route
     * (pricers/script_route.hpp): delta and rho adjoint, vega the parallel
     * shift of the whole grid (per unit of volatility), gamma and theta
     * central differences with common random numbers.
     */
    PricingResult price_local_vol_mc(const LocalVolInput &in);

} // namespace quantModeling

#endif // PRICERS_ADAPTERS_EQUITY_VANILLA_LV_HPP
