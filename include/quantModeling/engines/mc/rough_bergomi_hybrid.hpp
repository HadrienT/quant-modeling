#ifndef ENGINE_MC_ROUGH_BERGOMI_HYBRID_HPP
#define ENGINE_MC_ROUGH_BERGOMI_HYBRID_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/rough_bergomi.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace quantModeling
{

    struct RoughBergomiSettings
    {
        std::size_t n_steps = 100;
        long long n_paths = 100000;
        std::uint64_t seed = 1;
    };

    struct RoughBergomiResult
    {
        Real price = 0.0;
        Real std_error = 0.0;
    };

    /**
     * European vanilla price under rough Bergomi via the first-order (kappa
     * = 1) hybrid scheme of Bennedsen, Lunde & Pakkanen ("Hybrid scheme for
     * Brownian semistationary processes", Finance and Stochastics, 2017),
     * applied to rough Bergomi as in McCrickerd & Pakkanen ("Turbocharging
     * Monte Carlo pricing for the rough Bergomi model", Quantitative
     * Finance 18(11), 2018, section 1.1) -- both fetched and read directly.
     *
     * The Volterra process W^alpha is non-Markovian (its kernel (t-u)^alpha
     * gives every past increment of the driving Brownian motion W1 a say in
     * the current value), so there is no step-by-step SDE recursion the way
     * there is for Heston or SABR. The hybrid scheme instead splits each
     * grid point's value into:
     *   - a "near" term: the exact stochastic integral of the (singular
     *     near u=t) kernel over the single most recent sub-interval, jointly
     *     Gaussian with that sub-interval's own W1 increment (needed both to
     *     drive the price and to feed later "far" terms) via the exact 2x2
     *     covariance matrix Bennedsen-Lunde-Pakkanen give for their first-
     *     order case (their eq. 3.5, specialized to kappa=1: no
     *     hypergeometric cross-terms remain, only Sigma_11 = dt, Sigma_12 =
     *     dt^(alpha+1)/(alpha+1), Sigma_22 = dt^(2*alpha+1)/(2*alpha+1));
     *   - a "far" term: a discrete convolution of every earlier W1 increment
     *     against weights b_k (McCrickerd & Pakkanen eq. 1.3) chosen to
     *     minimise the L2 error of approximating the (smooth, away from the
     *     origin) kernel by a step function on each already-elapsed
     *     interval.
     *
     * The reference implementations evaluate this convolution via FFT for
     * O(n log n) per path; this implementation instead sums it directly
     * (O(n^2) per path) -- the exact same formula and result, just without
     * the added complexity of introducing an FFT dependency for a
     * chantier-sized number of time steps (a few hundred at most). This is
     * a deliberate performance/complexity trade-off, not an approximation:
     * revisit if profiling ever shows path counts or step counts large
     * enough for the asymptotic difference to matter.
     *
     * The log-forward step itself (given the variance path) uses a
     * standard left-point (Euler) discretisation of the model's stochastic
     * exponential -- the model does not need an exact scheme there (unlike
     * the variance process), since the hard, non-Markovian part is
     * entirely in simulating V, not in advancing the log-price given V.
     *
     * forward_variance_curve supplies xi_0(t) = E[V_t] -- in practice, the
     * ATM total variance curve derived from an already-calibrated vol
     * surface (e.g. the SVI pipeline this project already has), rather than
     * a free model parameter the way Heston's theta/v0 are.
     */
    RoughBergomiResult rough_bergomi_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const RoughBergomiParams &params,
        const std::function<Real(Real)> &forward_variance_curve,
        bool is_call,
        const RoughBergomiSettings &settings = {});

} // namespace quantModeling

#endif
