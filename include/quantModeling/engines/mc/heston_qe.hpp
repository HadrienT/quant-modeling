#ifndef ENGINE_MC_HESTON_QE_HPP
#define ENGINE_MC_HESTON_QE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/heston.hpp"

#include <cstddef>
#include <cstdint>

namespace quantModeling
{

    struct HestonQESettings
    {
        std::size_t n_steps = 32; ///< time steps over [0, ttm]; the QE variance step is designed to be accurate even for a handful of large steps, but the log-price step's own discretization (32) is only first-order in dt
        long long n_paths = 100000;
        std::uint64_t seed = 1;
        Real psi_c = 1.5;  ///< Andersen's recommended switching level, in [1,2]
        Real gamma1 = 0.5; ///< central discretization (gamma1 = gamma2 = 1/2) -- what Andersen uses in his own numerical tests
        Real gamma2 = 0.5;
    };

    struct HestonQEResult
    {
        Real price = 0.0;
        Real std_error = 0.0;
    };

    /**
     * European vanilla price under Heston via Andersen's QE-M scheme
     * ("Efficient Simulation of the Heston Stochastic Volatility Model",
     * Journal of Computational Finance 11(3), 2008 -- sections 3.2, 4.2, 4.3):
     * an (almost) exact simulation of the variance process by moment-matching
     * a squared-Gaussian or an exponential-tailed distribution depending on
     * how large the variance's own coefficient of variation is (the psi <=
     * psi_c switching rule), combined with a martingale-corrected log-price
     * step (the "-M" in QE-M) that keeps forward = E[F_T] exactly, in
     * expectation, at every time step -- not just in the continuous-time
     * limit.
     *
     * Works in the T-forward measure (dF/F = sqrt(v) dW1, no drift term),
     * matching models/equity/heston.hpp's convention: Andersen's own X
     * process is explicitly defined as a driftless martingale too ("adding a
     * drift to X is trivial and is omitted for notational simplicity"), so
     * this is not a special case or approximation -- it's exactly the
     * process the paper's own K0..K4 formulas are derived for.
     *
     * If rho > 0, the martingale correction's regularity condition
     * (Andersen's eq. 41) can in principle fail for very large time steps;
     * this implementation detects that per step and silently falls back to
     * the un-corrected K0 for that step rather than producing a NaN/inf
     * price (the paper notes this is a large-step, positive-correlation
     * edge case that virtually never binds in practice: roughly
     * rho*xi*dt < 2).
     */
    HestonQEResult heston_qe_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const HestonParams &params, bool is_call,
        const HestonQESettings &settings = {});

} // namespace quantModeling

#endif
