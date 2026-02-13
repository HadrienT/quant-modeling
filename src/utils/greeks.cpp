#include "quantModeling/utils/greeks.hpp"
#include <cmath>

namespace quantModeling
{
    Greeks compute_mc_greeks(
        const std::function<Real(Real, Real, Real, Real)> &pricing_fn,
        Real S0, Real vol, Real r, Real T,
        const GreeksBumps &bumps)
    {
        Greeks greeks;

        // Base price
        const Real price_base = pricing_fn(S0, vol, r, T);

        // ===== Delta =====
        // Bump spot: central difference
        const Real price_S_up = pricing_fn(S0 * (1.0 + bumps.delta_bump), vol, r, T);
        const Real price_S_dn = pricing_fn(S0 * (1.0 - bumps.delta_bump), vol, r, T);
        const Real dS = S0 * 2.0 * bumps.delta_bump;
        greeks.delta = (price_S_up - price_S_dn) / dS;

        // ===== Gamma =====
        // Second derivative: (f(S+h) - 2*f(S) + f(S-h)) / h^2
        const Real dS_gamma = S0 * bumps.delta_bump;
        greeks.gamma = (price_S_up - 2.0 * price_base + price_S_dn) / (dS_gamma * dS_gamma);

        // ===== Vega =====
        // Bump volatility: central difference
        const Real price_vol_up = pricing_fn(S0, vol * (1.0 + bumps.vega_bump), r, T);
        const Real price_vol_dn = pricing_fn(S0, vol * (1.0 - bumps.vega_bump), r, T);
        const Real dvol = vol * 2.0 * bumps.vega_bump;
        greeks.vega = (price_vol_up - price_vol_dn) / dvol;

        // ===== Rho =====
        // Bump rate: central difference
        const Real price_r_up = pricing_fn(S0, vol, r + bumps.rho_bump, T);
        const Real price_r_dn = pricing_fn(S0, vol, r - bumps.rho_bump, T);
        const Real dr = 2.0 * bumps.rho_bump;
        greeks.rho = (price_r_up - price_r_dn) / dr;

        // ===== Theta =====
        // Theta as negative price change when time passes (T decreases)
        // theta = -(dPrice/dT) ≈ (Price(T-dt) - Price(T+dt)) / (2*dt)
        const Real T_down = std::max(1e-8, T - bumps.theta_bump);
        const Real T_up = T + bumps.theta_bump;
        const Real price_T_dn = pricing_fn(S0, vol, r, T_down);
        const Real price_T_up = pricing_fn(S0, vol, r, T_up);
        const Real dT = 2.0 * bumps.theta_bump;
        // Note: theta_per_year = (f(T-dt) - f(T+dt)) / (2*dt)
        // This gives theta per year (since bumps.theta_bump is in years)
        greeks.theta = (price_T_dn - price_T_up) / dT;

        return greeks;
    }

} // namespace quantModeling
