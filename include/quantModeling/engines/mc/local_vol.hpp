#ifndef ENGINE_MC_LOCAL_VOL_HPP
#define ENGINE_MC_LOCAL_VOL_HPP

#include "quantModeling/core/results.hpp"
#include "quantModeling/pricers/inputs.hpp"

namespace quantModeling
{

/**
 * @brief Euler-Maruyama Monte-Carlo pricer under a Dupire local-vol surface.
 *
 * The local-vol surface is provided as a pre-evaluated K×T grid (from the
 * Python calibration pipeline).  This engine bilinearly interpolates
 * σ_loc(S_t, t) at each time step.
 *
 * Greeks are computed via Common Random Numbers (CRN) finite differences:
 *   Δ, Γ  — central spot bumps (±1% spot)
 *   Θ     — forward maturity reduction (−1 day)
 *   ρ     — upward rate bump (+1 bp)
 *   vega  — parallel +1% additive shift on the entire σ_loc grid
 *
 * Antithetic variates halve variance at no extra simulation cost.
 */
PricingResult price_local_vol_mc(const LocalVolInput &in);

} // namespace quantModeling

#endif // ENGINE_MC_LOCAL_VOL_HPP
