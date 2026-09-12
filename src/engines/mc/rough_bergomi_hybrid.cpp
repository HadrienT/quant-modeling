#include "quantModeling/engines/mc/rough_bergomi_hybrid.hpp"

#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace quantModeling
{

    namespace
    {

        /// b_k weights for the hybrid scheme's far-field sum (McCrickerd &
        /// Pakkanen eq. 1.3): the point within each already-elapsed
        /// sub-interval [(k-1)/n, k/n] at which the (smooth away from the
        /// origin) kernel x -> x^alpha is evaluated, chosen to minimise the
        /// L2 discretisation error.
        std::vector<Real> hybrid_b_weights(std::size_t n_steps, Real alpha)
        {
            std::vector<Real> b(n_steps + 1, 0.0); // b[k] for k = 2..n_steps; b[0], b[1] unused
            for (std::size_t k = 2; k <= n_steps; ++k)
            {
                const Real kk = static_cast<Real>(k);
                const Real base = (std::pow(kk, alpha + 1.0) - std::pow(kk - 1.0, alpha + 1.0)) / (alpha + 1.0);
                b[k] = std::pow(base, 1.0 / alpha);
            }
            return b;
        }

        Real normal_draw(Pcg32 &rng)
        {
            return inverse_normal_cdf(uniform01(rng));
        }

    } // namespace

    RoughBergomiResult rough_bergomi_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const RoughBergomiParams &params,
        const std::function<Real(Real)> &forward_variance_curve,
        bool is_call,
        const RoughBergomiSettings &settings)
    {
        RoughBergomiResult result;
        if (ttm <= 0.0 || settings.n_paths <= 0 || settings.n_steps == 0)
            return result;

        const Real alpha = rough_bergomi_alpha(params);
        const Real dt = ttm / static_cast<Real>(settings.n_steps);
        const std::size_t n = settings.n_steps;

        // Joint law of (dW1_i, Y_i) over one sub-interval of length dt --
        // Bennedsen-Lunde-Pakkanen eq. (3.5) specialised to their kappa = 1
        // case (j = k = 2, so the general formula's hypergeometric
        // cross-terms don't arise): dW1_i is the plain Brownian increment,
        // Y_i is the exact "near" Volterra integral over the same interval.
        const Real sigma11 = dt;
        const Real sigma12 = std::pow(dt, alpha + 1.0) / (alpha + 1.0);
        const Real sigma22 = std::pow(dt, 2.0 * alpha + 1.0) / (2.0 * alpha + 1.0);
        const Real l11 = std::sqrt(sigma11);
        const Real l21 = sigma12 / l11;
        const Real l22 = std::sqrt(std::max(sigma22 - l21 * l21, Real(0.0)));

        const std::vector<Real> b = hybrid_b_weights(n, alpha);
        std::vector<Real> b_pow_alpha(n + 1, 0.0);
        for (std::size_t k = 2; k <= n; ++k)
            b_pow_alpha[k] = std::pow(b[k], alpha);
        const Real dt_pow_alpha = std::pow(dt, alpha);
        const Real sqrt_2alpha_plus_1 = std::sqrt(2.0 * alpha + 1.0);

        RngFactory factory(settings.seed);
        Pcg32 rng = factory.make(0);

        WelfordAccumulator acc;
        std::vector<Real> dW1(n + 1, 0.0); // dW1[i], i = 1..n

        for (long long path = 0; path < settings.n_paths; ++path)
        {
            Real v_prev = forward_variance_curve(0.0);
            Real log_f = std::log(forward);

            for (std::size_t i = 1; i <= n; ++i)
            {
                const Real z_a = normal_draw(rng);
                const Real z_b = normal_draw(rng);
                const Real dw1_i = l11 * z_a;
                const Real y_i = l21 * z_a + l22 * z_b;
                dW1[i] = dw1_i;

                // Advance the log-forward over [t_{i-1}, t_i] using the
                // variance already known at t_{i-1} (left-point Euler --
                // see the header doc comment).
                const Real z2 = normal_draw(rng);
                const Real dz_i = params.rho * dw1_i + std::sqrt(std::max(1.0 - params.rho * params.rho, 0.0)) * std::sqrt(dt) * z2;
                log_f += std::sqrt(std::max(v_prev, Real(0.0))) * dz_i - 0.5 * v_prev * dt;

                // Far-field convolution: every strictly-past increment,
                // weighted by the hybrid scheme's b_k. O(n) per step, O(n^2)
                // per path in total -- see the header doc comment.
                Real far_sum = 0.0;
                for (std::size_t k = 2; k <= i; ++k)
                    far_sum += b_pow_alpha[k] * dW1[i - k + 1];

                const Real w_tilde_i = sqrt_2alpha_plus_1 * (y_i + dt_pow_alpha * far_sum);
                const Real t_i = static_cast<Real>(i) * dt;
                v_prev = forward_variance_curve(t_i) *
                         std::exp(params.eta * w_tilde_i - 0.5 * params.eta * params.eta * std::pow(t_i, 2.0 * alpha + 1.0));
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
