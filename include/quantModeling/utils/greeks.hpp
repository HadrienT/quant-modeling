#ifndef UTILS_GREEKS_HPP
#define UTILS_GREEKS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/core/results.hpp"
#include <functional>
#include <optional>

namespace quantModeling
{
    /**
     * Parameters controlling bump sizes for finite-difference Greeks computation
     */
    struct GreeksBumps
    {
        Real delta_bump = 0.01;        // 1% spot bumps
        Real vega_bump = 0.001;        // 0.1% vol bumps
        Real rho_bump = 0.0001;        // 0.01% rate bumps
        Real theta_bump = 1.0 / 365.0; // 1 day
    };

    /**
     * Compute Greeks via central finite differences on a pricing function
     *
     * @param pricing_fn Lambda/function that computes NPV given (S0, vol, r, T)
     *                   Signature: Real(Real S0, Real vol, Real r, Real T)
     * @param S0 Current spot price
     * @param vol Current volatility
     * @param r Current risk-free rate
     * @param T Time to maturity
     * @param bumps Bump sizes for finite differences (default: GreeksBumps())
     * @return Greeks struct with delta, gamma, vega, rho, theta computed via central differences
     */
    Greeks compute_mc_greeks(
        const std::function<Real(Real, Real, Real, Real)> &pricing_fn,
        Real S0, Real vol, Real r, Real T,
        const GreeksBumps &bumps = GreeksBumps());

} // namespace quantModeling

#endif
