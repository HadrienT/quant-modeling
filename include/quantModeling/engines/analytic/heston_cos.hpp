#ifndef ENGINE_ANALYTIC_HESTON_COS_HPP
#define ENGINE_ANALYTIC_HESTON_COS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/heston.hpp"

#include <cstddef>

namespace quantModeling
{

    struct HestonCOSSettings
    {
        std::size_t n_terms = 160; ///< N in Fang & Oosterlee's cosine-series truncation; convergence is exponential in N for a smooth density
        Real truncation_L = 10.0;  ///< integration-range half-width in cumulant-std-dev units; (49) in the paper, accurate for ttm in [0.1, 10]
        Real cumulant_step = 1e-4; ///< finite-difference step for the numerically-estimated cumulants -- see heston_cos_price's doc comment
    };

    /**
     * European vanilla price under Heston via the COS method (Fang &
     * Oosterlee, "A Novel Pricing Method for European Options Based on
     * Fourier-Cosine Series Expansions", SIAM J. Sci. Comput. 31, 2008):
     * truncate the log-return domain to [a,b], replace the (unknown)
     * transition density by its Fourier-cosine series computed from the
     * (known) characteristic function, and integrate the payoff against
     * that series termwise -- exponential convergence in the number of
     * terms for a smooth density, versus the polynomial convergence of a
     * naive FFT/Carr-Madan approach.
     *
     * Deliberately prices puts directly and recovers calls via put-call
     * parity (call = put + discount_factor*(forward-strike)), per the
     * paper's own Remark 5.2: a call's payoff grows exponentially in the
     * log-forward coordinate the series is built in, which introduces a
     * cancellation error that a bounded put payoff does not suffer from.
     *
     * The truncation range [a,b] = [x + c1 - L*sqrt(c2), x + c1 + L*sqrt(c2)]
     * (x = ln(forward/strike)) needs the first two cumulants, c1 and c2, of
     * the log-return distribution. Fang & Oosterlee's Appendix A gives a
     * closed form for Heston's c1 and c2 (and none at all for c4, which
     * (49) also calls for) -- but that c2 expression is long, dense, and a
     * poor candidate for hand-transcription without a second independent
     * source to check it against. Instead, c1 and c2 are estimated here by
     * central finite differences of log(phi(u)) at u=0 (phi(0)=1 always, so
     * log(phi(0))=0 exactly, and c_n = the n-th derivative of log(phi) at 0
     * divided by i^n): this is exact for whatever characteristic function
     * is actually implemented, self-consistent by construction (an error in
     * heston_log_return_characteristic_function would show up identically
     * in both the pricing sum and the range it is integrated over), and
     * generalizes to any future characteristic-function-based model reusing
     * this truncation-range logic, not just Heston. It costs four extra
     * characteristic-function evaluations per pricing call; the truncation
     * range only needs to be roughly right; L already provides a generous
     * margin.
     */
    Real heston_cos_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const HestonParams &params, bool is_call,
        const HestonCOSSettings &settings = {});

} // namespace quantModeling

#endif
