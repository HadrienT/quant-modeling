#ifndef EQUITY_SABR_HPP
#define EQUITY_SABR_HPP

#include "quantModeling/core/types.hpp"

namespace quantModeling
{

    /**
     * SABR model parameters (Hagan, Kumar, Lesniewski, Woodward, "Managing
     * Smile Risk", Wilmott 2002):
     *
     *   dF = alpha * F^beta * dW1
     *   dalpha = nu * alpha * dW2
     *   dW1 dW2 = rho dt
     *
     * beta is conventionally fixed exogenously (equity/FX often 1, rates
     * often 0.5) rather than calibrated: a single smile snapshot cannot
     * jointly identify alpha and beta (they trade off almost exactly against
     * each other), so market/sabr_calibration.hpp only fits alpha, rho, nu.
     */
    struct SABRParams
    {
        Real alpha = 0.2;
        Real beta = 0.5;
        Real rho = 0.0;
        Real nu = 0.3;
    };

    /**
     * Hagan's 2002 closed-form Black implied volatility -- the standard
     * asymptotic (singular perturbation) formula every SABR calibration
     * starts from. Known to produce negative risk-neutral densities for low
     * strikes when beta is away from 1 and/or nu is large and/or maturity is
     * long; market/sabr_pde.hpp's arbitrage-free solver is what to use when
     * that matters (CMS, digitals, deep wings). Guards the z -> 0 removable
     * singularity (z/x(z) -> 1) directly on z rather than on K == F: z is
     * also exactly 0 whenever nu == 0, at every strike, not only at the
     * money.
     */
    Real sabr_implied_vol(Real forward, Real strike, Real ttm, const SABRParams &p) noexcept;

    /// Black-76 European call price: forward, strike, volatility, maturity,
    /// and the discount factor to today (price = discount * E[(F_T-K)^+]).
    Real black76_call_price(Real forward, Real strike, Real ttm, Real vol, Real discount_factor) noexcept;

    /// Black-76 vega: d(call price)/d(vol).
    Real black76_vega(Real forward, Real strike, Real ttm, Real vol, Real discount_factor) noexcept;

    /**
     * Implied volatility from a Black-76 call price via Newton-Raphson with
     * vega, a handful of bisection fallback steps if Newton misbehaves.
     * Returns NaN if the price is outside no-arbitrage bounds
     * (intrinsic value, discount * forward) or the iteration does not
     * converge -- a documented failure, not a silently wrong guess.
     */
    Real black76_implied_vol(Real price, Real forward, Real strike, Real ttm, Real discount_factor) noexcept;

} // namespace quantModeling

#endif
