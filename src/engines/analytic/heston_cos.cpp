#include "quantModeling/engines/analytic/heston_cos.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace quantModeling
{

    namespace
    {

        using C = std::complex<Real>;

        /// Cosine series coefficients chi_k(c,d) and psi_k(c,d) of e^y and 1
        /// respectively, on [c,d] subset [a,b] -- Fang & Oosterlee eqs (22)-(23).
        Real chi_k(std::size_t k, Real c, Real d, Real a, Real b)
        {
            const Real kpi_ba = static_cast<Real>(k) * std::numbers::pi_v<Real> / (b - a);
            const Real cos_d = std::cos(kpi_ba * (d - a));
            const Real cos_c = std::cos(kpi_ba * (c - a));
            const Real sin_d = std::sin(kpi_ba * (d - a));
            const Real sin_c = std::sin(kpi_ba * (c - a));
            return (cos_d * std::exp(d) - cos_c * std::exp(c) + kpi_ba * sin_d * std::exp(d) -
                    kpi_ba * sin_c * std::exp(c)) /
                   (1.0 + kpi_ba * kpi_ba);
        }

        Real psi_k(std::size_t k, Real c, Real d, Real a, Real b)
        {
            if (k == 0)
                return d - c;
            const Real kpi_ba = static_cast<Real>(k) * std::numbers::pi_v<Real> / (b - a);
            return (std::sin(kpi_ba * (d - a)) - std::sin(kpi_ba * (c - a))) / kpi_ba;
        }

        /// Payoff series coefficients U_k for a put (Fang & Oosterlee eq 29,
        /// K factored out -- multiplied in by the caller).
        Real put_u_k(std::size_t k, Real a, Real b)
        {
            return (2.0 / (b - a)) * (-chi_k(k, a, 0.0, a, b) + psi_k(k, a, 0.0, a, b));
        }

        /// First two cumulants of the log-return, estimated by central finite
        /// differences of log(phi(u)) at u=0 -- see heston_cos_price's doc
        /// comment for why this is preferred over Fang & Oosterlee's
        /// closed-form Heston cumulant table.
        std::pair<Real, Real> estimate_cumulants(Real ttm, const HestonParams &params, Real h)
        {
            const C g_plus = std::log(heston_log_return_characteristic_function(h, ttm, params));
            const C g_minus = std::log(heston_log_return_characteristic_function(-h, ttm, params));

            const C i(0.0, 1.0);
            const Real c1 = (-i * (g_plus - g_minus) / (2.0 * h)).real();
            const Real c2 = (-(g_plus + g_minus) / (h * h)).real(); // g(0) = log(phi(0)) = 0 exactly

            return {c1, c2};
        }

    } // namespace

    Real heston_cos_price(
        Real forward, Real strike, Real ttm, Real discount_factor,
        const HestonParams &params, bool is_call,
        const HestonCOSSettings &settings)
    {
        const Real x = std::log(forward / strike);

        const auto [c1, c2] = estimate_cumulants(ttm, params, settings.cumulant_step);
        const Real half_width = settings.truncation_L * std::sqrt(std::max(c2, Real(1e-12)));
        const Real a = x + c1 - half_width;
        const Real b = x + c1 + half_width;

        Real sum = 0.0;
        for (std::size_t k = 0; k < settings.n_terms; ++k)
        {
            const Real omega = static_cast<Real>(k) * std::numbers::pi_v<Real> / (b - a);
            const C phi = heston_log_return_characteristic_function(omega, ttm, params);
            const C rotation = std::exp(C(0.0, omega * (x - a)));
            const Real term = (phi * rotation).real() * put_u_k(k, a, b);
            sum += (k == 0 ? 0.5 : 1.0) * term; // the cosine series' k=0 term is weighted by one-half
        }

        const Real put_price = discount_factor * strike * sum;
        const Real price = is_call ? put_price + discount_factor * (forward - strike) : put_price;
        return std::max(price, Real(0.0)); // guards against small negative numerical noise from the truncated series
    }

} // namespace quantModeling
