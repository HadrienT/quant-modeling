#include "quantModeling/models/equity/heston.hpp"

namespace quantModeling
{

    bool feller_condition_satisfied(const HestonParams &p) noexcept
    {
        return 2.0 * p.kappa * p.theta > p.xi * p.xi;
    }

    std::complex<Real> heston_log_return_characteristic_function(
        Real u, Real ttm, const HestonParams &p) noexcept
    {
        using C = std::complex<Real>;
        constexpr C i(0.0, 1.0);

        const C beta = p.kappa - i * p.rho * p.xi * u; // shorthand, appears repeatedly below
        const C d_squared_minus_beta_squared = p.xi * p.xi * C(u * u, u);
        const C D = std::sqrt(beta * beta + d_squared_minus_beta_squared);
        const C beta_plus_D = beta + D;

        // beta - D is computed via beta^2 - D^2 = -d_squared_minus_beta_squared
        // rather than as a direct subtraction: for small xi, beta and D are
        // both within O(xi^2) of each other (D -> beta as xi -> 0), so a
        // direct subtraction loses precision catastrophically once xi drops
        // much below 1e-3 -- verified empirically (tests/testHestonCOS.cpp
        // hit prices off by 5+ orders of magnitude before this fix). This
        // form is a division of two well-scaled quantities instead, with no
        // cancellation.
        const C beta_minus_D = -d_squared_minus_beta_squared / beta_plus_D;
        const C G = beta_minus_D / beta_plus_D;

        const C exp_mDt = std::exp(-D * ttm);
        const C term_v0 = (p.v0 / (p.xi * p.xi)) * beta_minus_D * (Real(1.0) - exp_mDt) / (Real(1.0) - G * exp_mDt);
        const C term_theta = (p.kappa * p.theta / (p.xi * p.xi)) *
                             (ttm * beta_minus_D - Real(2.0) * std::log((Real(1.0) - G * exp_mDt) / (Real(1.0) - G)));

        return std::exp(term_v0 + term_theta);
    }

} // namespace quantModeling
