#ifndef EQUITY_HESTON_HPP
#define EQUITY_HESTON_HPP

#include "quantModeling/core/types.hpp"

#include <complex>

namespace quantModeling
{

    /**
     * Heston (1993) stochastic volatility model, under the T-forward measure
     * (no drift term: dF/F = sqrt(v) dW1, matching the Black-76-style
     * forward/discount-factor convention used by models/equity/sabr.hpp):
     *
     *   dF_t/F_t = sqrt(v_t) dW1_t
     *   dv_t = kappa*(theta - v_t) dt + xi*sqrt(v_t) dW2_t, v_0 = v0
     *   dW1_t dW2_t = rho dt
     *
     * kappa: mean-reversion speed, theta: long-run variance, xi: vol of
     * vol, rho: correlation, v0: initial variance.
     */
    struct HestonParams
    {
        Real v0 = 0.04;
        Real kappa = 1.5;
        Real theta = 0.04;
        Real xi = 0.3;
        Real rho = -0.6;
    };

    /**
     * Feller condition 2*kappa*theta > xi^2: the variance process cannot
     * reach zero. Informational only -- heston_log_return_characteristic_
     * function stays valid (no discontinuity, see its doc comment) whether
     * or not this holds; a calibration that lands outside it is a modeling
     * caveat worth surfacing to the caller, not a reason to reject the fit.
     */
    bool feller_condition_satisfied(const HestonParams &p) noexcept;

    /**
     * Characteristic function of the log forward return, ln(F_T/F_0), under
     * Heston -- i.e. E[exp(iu * ln(F_T/F_0))]. Excludes any log(F_0) or
     * log(K) term (those are applied separately by the caller, e.g.
     * engines/analytic/heston_cos.hpp), matching Fang & Oosterlee's
     * phi_hes(omega; u0) := phi(omega; 0, u0) convention.
     *
     * Uses the "little trap" safe form (Albrecher, Mayer, Schoutens,
     * Tistaert, "The Little Heston Trap", Wilmott Magazine, Jan 2007; the
     * same form appears in Fang & Oosterlee 2008, section 3.3): of the two
     * algebraically equivalent closed forms for this characteristic
     * function, one of them (the one found in Heston's original 1993 paper)
     * crosses the negative real axis as u increases when the principal
     * branch of sqrt/log is used, causing a discontinuity and potential
     * mispricing; the other does not. This implementation is verified
     * against both of those sources (fetched and read directly, not
     * transcribed from memory), which independently state the identical
     * formula for the safe form.
     *
     * The safe form relies on std::sqrt(std::complex<Real>) returning the
     * root with non-negative real part, and std::log(std::complex<Real>)
     * using the principal branch (Im in (-pi, pi]) -- exactly the two
     * branch conventions both sources require, and exactly what the C++
     * standard mandates for <complex>, so no manual branch-cut handling is
     * needed here.
     */
    std::complex<Real> heston_log_return_characteristic_function(
        Real u, Real ttm, const HestonParams &p) noexcept;

} // namespace quantModeling

#endif
