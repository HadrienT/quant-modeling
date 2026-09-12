#ifndef EQUITY_ROUGH_BERGOMI_HPP
#define EQUITY_ROUGH_BERGOMI_HPP

#include "quantModeling/core/types.hpp"

namespace quantModeling
{

    /**
     * Rough Bergomi (Bayer, Friz, Gatheral, "Pricing under rough volatility",
     * Quantitative Finance 16(6), 2016). Under the T-forward measure:
     *
     *   F_t = E( int_0^t sqrt(V_u) d(rho*W1_u + sqrt(1-rho^2)*W2_u) )_t
     *   V_t = xi_0(t) * exp(eta*W^alpha_t - 0.5*eta^2*t^(2*alpha+1))
     *
     * where E(.) is the stochastic exponential, xi_0(t) = E[V_t] is the
     * forward variance curve (the analogue of a forward-rate curve for
     * variance -- see engines/mc/rough_bergomi_hybrid.hpp for how it's
     * supplied), and W^alpha is the Riemann-Liouville Volterra process
     *
     *   W^alpha_t = sqrt(2*alpha+1) * int_0^t (t-u)^alpha dW1_u,  alpha in (-1/2, 0).
     *
     * There is no mean-reversion parameter (no kappa, unlike Heston): the
     * entire term structure of variance lives in xi_0(.) directly, and the
     * Volterra kernel (t-u)^alpha alone is what makes the model "rough" --
     * for alpha -> 0 (H -> 1/2), W^alpha coincides with standard Brownian
     * motion; alpha close to -1/2 (H close to 0) produces the empirically-
     * observed explosive short-maturity ATM skew that Heston/SABR cannot
     * reproduce without unrealistic parameters.
     *
     * H is the Hurst exponent, related to alpha by alpha = H - 1/2; the
     * project works in terms of H (the conventional name for the roughness
     * parameter) and converts to alpha only where the source formulas are
     * naturally expressed in alpha (engines/mc/rough_bergomi_hybrid.cpp).
     */
    struct RoughBergomiParams
    {
        Real H = 0.1;    ///< Hurst exponent, in (0, 1/2); smaller = rougher. Equity-calibrated values are typically 0.05-0.15.
        Real eta = 1.9;  ///< vol-of-vol, plays the role of xi in Heston or nu in SABR
        Real rho = -0.7; ///< correlation between the variance-driving and price-driving Brownian motions
    };

    /// alpha = H - 1/2, the exponent the Volterra kernel and the hybrid
    /// simulation scheme (Bennedsen, Lunde, Pakkanen 2017) are naturally
    /// expressed in.
    inline Real rough_bergomi_alpha(const RoughBergomiParams &p) noexcept
    {
        return p.H - 0.5;
    }

} // namespace quantModeling

#endif
