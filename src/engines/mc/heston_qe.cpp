#include "quantModeling/engines/mc/heston_qe.hpp"

#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    namespace
    {

        /// One QE step for the variance process: draws V(t+dt) given V(t),
        /// and reports which branch was used plus the branch's own
        /// constants, so the caller can compute the matching martingale
        /// correction (Andersen's Proposition 9) without recomputing psi.
        struct QEStep
        {
            Real v_next = 0.0;
            bool used_quadratic = false;
            Real a = 0.0, b_squared = 0.0; // quadratic branch (psi <= psi_c)
            Real p = 0.0, beta = 0.0;      // exponential branch (psi > psi_c)
        };

        QEStep qe_variance_step(Real v_t, Real dt, const HestonParams &params, Real psi_c, Pcg32 &rng)
        {
            const Real e_kt = std::exp(-params.kappa * dt);
            const Real m = params.theta + (v_t - params.theta) * e_kt;
            const Real s2 = v_t * params.xi * params.xi * e_kt / params.kappa * (1.0 - e_kt) +
                            params.theta * params.xi * params.xi / (2.0 * params.kappa) * (1.0 - e_kt) * (1.0 - e_kt);
            const Real psi = s2 / (m * m);

            const Real u_v = uniform01(rng);
            QEStep step;

            if (psi <= psi_c)
            {
                const Real inv_psi = 1.0 / psi;
                step.b_squared = 2.0 * inv_psi - 1.0 + std::sqrt(2.0 * inv_psi) * std::sqrt(2.0 * inv_psi - 1.0);
                step.a = m / (1.0 + step.b_squared);
                step.used_quadratic = true;

                const Real z_v = inverse_normal_cdf(u_v);
                const Real b = std::sqrt(step.b_squared);
                step.v_next = step.a * (b + z_v) * (b + z_v);
            }
            else
            {
                step.p = (psi - 1.0) / (psi + 1.0);
                step.beta = 2.0 / (m * (psi + 1.0));
                step.used_quadratic = false;

                step.v_next = (u_v <= step.p) ? 0.0 : std::log((1.0 - step.p) / (1.0 - u_v)) / step.beta;
            }
            return step;
        }

    } // namespace

    HestonQEResult heston_qe_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const HestonParams &params, bool is_call,
        const HestonQESettings &settings)
    {
        HestonQEResult result;
        if (ttm <= 0.0 || settings.n_paths <= 0 || settings.n_steps == 0)
            return result;

        const Real dt = ttm / static_cast<Real>(settings.n_steps);
        const Real kappa = params.kappa, theta = params.theta, xi = params.xi, rho = params.rho;
        const Real g1 = settings.gamma1, g2 = settings.gamma2;

        // Andersen eq. (33): constants for the log-forward step, fixed
        // across steps since dt is fixed.
        const Real K0 = -rho * kappa * theta / xi * dt;
        const Real K1 = g1 * dt * (kappa * rho / xi - 0.5) - rho / xi;
        const Real K2 = g2 * dt * (kappa * rho / xi - 0.5) + rho / xi;
        const Real K3 = g1 * dt * (1.0 - rho * rho);
        const Real K4 = g2 * dt * (1.0 - rho * rho);
        const Real A = K2 + 0.5 * K4; // Andersen eq. (35)'s exponent

        RngFactory factory(settings.seed);
        Pcg32 rng = factory.make(0);

        WelfordAccumulator acc;
        const Real log_forward = std::log(forward);

        for (long long path = 0; path < settings.n_paths; ++path)
        {
            Real v = params.v0;
            Real log_f = log_forward;

            for (std::size_t step_idx = 0; step_idx < settings.n_steps; ++step_idx)
            {
                const QEStep step = qe_variance_step(v, dt, params, settings.psi_c, rng);

                // Martingale correction (Andersen Proposition 9): M is the
                // moment generating function of v_next at A, evaluated in
                // closed form for whichever branch was used.
                Real k0_star = K0;
                if (step.used_quadratic && A < 1.0 / (2.0 * step.a))
                {
                    const Real one_minus_2Aa = 1.0 - 2.0 * A * step.a;
                    const Real log_m = (A * step.b_squared * step.a) / one_minus_2Aa - 0.5 * std::log(one_minus_2Aa);
                    k0_star = -log_m - (K1 + 0.5 * K3) * v;
                }
                else if (!step.used_quadratic && A < step.beta)
                {
                    const Real log_m = std::log(step.beta * (1.0 - step.p) / (step.beta - A));
                    k0_star = -log_m - (K1 + 0.5 * K3) * v;
                }
                // else: Andersen's regularity condition (41)/(43) fails for
                // this step (only possible for rho > 0 and a large step,
                // see the header doc comment) -- fall back to the
                // un-corrected K0 rather than a NaN/inf log-price.

                const Real z = inverse_normal_cdf(uniform01(rng));
                const Real variance_term = K3 * v + K4 * step.v_next;
                log_f += k0_star + K1 * v + K2 * step.v_next + std::sqrt(std::max(variance_term, Real(0.0))) * z;

                v = step.v_next;
            }

            const Real f_t = std::exp(log_f);
            const Real payoff = is_call ? std::max(f_t - strike, Real(0.0)) : std::max(strike - f_t, Real(0.0));
            acc.add(discount_factor * payoff);
        }

        result.price = acc.mean;
        result.std_error = acc.std_error();
        return result;
    }

} // namespace quantModeling
