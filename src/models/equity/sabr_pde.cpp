#include "quantModeling/models/equity/sabr_pde.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    namespace
    {

        Real gamma_of_K(Real K, Real f, Real beta) noexcept
        {
            if (std::fabs(K - f) < 1e-10 * std::max(f, Real(1.0)))
                return beta * std::pow(f, beta - 1.0); // K -> f limit
            return (std::pow(K, beta) - std::pow(f, beta)) / (K - f);
        }

        Real y_of_K(Real K, Real f, Real beta) noexcept
        {
            const Real one_minus_beta = 1.0 - beta;
            if (std::fabs(one_minus_beta) < 1e-10)
                return std::log(K / f); // beta -> 1 limit
            return (std::pow(K, one_minus_beta) - std::pow(f, one_minus_beta)) / one_minus_beta;
        }

        Real D_of_K(Real K, Real f, const SABRParams &p) noexcept
        {
            const Real y = y_of_K(K, f, p.beta);
            const Real inside = p.alpha * p.alpha + 2.0 * p.alpha * p.rho * p.nu * y + p.nu * p.nu * y * y;
            return std::sqrt(std::max(inside, Real(0.0))) * std::pow(K, p.beta);
        }

        /// sigma^2(T, K) = D(K)^2 * E(T, K) -- the PDE's diffusion coefficient.
        Real diffusion_coefficient_squared(Real T, Real K, Real f, const SABRParams &p) noexcept
        {
            const Real D = D_of_K(K, f, p);
            const Real E = std::exp(p.rho * p.nu * p.alpha * gamma_of_K(K, f, p.beta) * T);
            return D * D * E;
        }

        /// Thomas algorithm for a tridiagonal system. sub[0] and sup[n-1] are
        /// never read (there is no x[-1] or x[n]).
        void solve_tridiagonal(const std::vector<Real> &sub, const std::vector<Real> &diag,
                               const std::vector<Real> &sup, const std::vector<Real> &rhs,
                               std::vector<Real> &x)
        {
            const std::size_t n = diag.size();
            std::vector<Real> c_prime(n), d_prime(n);

            c_prime[0] = sup[0] / diag[0];
            d_prime[0] = rhs[0] / diag[0];
            for (std::size_t i = 1; i < n; ++i)
            {
                const Real m = diag[i] - sub[i] * c_prime[i - 1];
                c_prime[i] = sup[i] / m;
                d_prime[i] = (rhs[i] - sub[i] * d_prime[i - 1]) / m;
            }

            x.assign(n, 0.0);
            x[n - 1] = d_prime[n - 1];
            for (std::size_t k = n - 1; k-- > 0;)
                x[k] = d_prime[k] - c_prime[k] * x[k + 1];
        }

        Real linear_interp(const std::vector<Real> &xs, const std::vector<Real> &ys, Real x) noexcept
        {
            if (x <= xs.front())
                return ys.front();
            if (x >= xs.back())
                return ys.back();
            const auto it = std::lower_bound(xs.begin(), xs.end(), x);
            const auto i1 = static_cast<std::size_t>(it - xs.begin());
            const std::size_t i0 = i1 - 1;
            const Real w = (x - xs[i0]) / (xs[i1] - xs[i0]);
            return (1.0 - w) * ys[i0] + w * ys[i1];
        }

    } // namespace

    SABRPDEResult sabr_arbitrage_free_prices(
        Real forward, Real ttm, const SABRParams &params,
        const std::vector<Real> &strikes, const SABRPDESettings &settings)
    {
        SABRPDEResult result;
        result.strikes = strikes;
        result.call_prices.assign(strikes.size(), 0.0);

        if (strikes.empty() || ttm <= 0.0)
            return result;

        const std::size_t n = std::max(settings.n_space, std::size_t(3));

        Real k_lo = forward, k_hi = forward;
        for (const Real K : strikes)
        {
            k_lo = std::min(k_lo, K);
            k_hi = std::max(k_hi, K);
        }

        // Grid range: a local-vol-scale margin around the requested strikes,
        // wide enough that the frozen-boundary approximation (see the header
        // doc comment) holds -- the option is already close to intrinsic
        // value / zero out there.
        const Real vol_scale = params.alpha * std::pow(forward, params.beta) * std::sqrt(ttm);
        const Real margin = settings.grid_std_devs * std::max(vol_scale, Real(1e-6));
        const Real K_min = std::max(Real(1e-6), std::min(k_lo, forward) - margin);
        const Real K_max = std::max(k_hi, forward) + margin;

        std::vector<Real> K_grid(n);
        const Real dK = (K_max - K_min) / static_cast<Real>(n - 1);
        for (std::size_t i = 0; i < n; ++i)
            K_grid[i] = K_min + static_cast<Real>(i) * dK;

        std::vector<Real> C(n);
        for (std::size_t i = 0; i < n; ++i)
            C[i] = std::max(forward - K_grid[i], Real(0.0));

        const Real left_boundary_value = C.front(); // frozen for all T -- see header doc comment
        const Real right_boundary_value = C.back(); // frozen for all T

        const Real dt = ttm / static_cast<Real>(settings.n_time);
        const std::size_t n_interior = n - 2; // grid indices 1..n-2

        std::vector<Real> sub(n_interior), diag(n_interior), sup(n_interior), rhs(n_interior), x(n_interior);

        for (std::size_t step = 0; step < settings.n_time; ++step)
        {
            const Real T_old = static_cast<Real>(step) * dt;
            const Real T_new = T_old + dt;
            const Real theta = (step < settings.n_rannacher_steps) ? Real(1.0) : Real(0.5);

            for (std::size_t j = 0; j < n_interior; ++j)
            {
                const std::size_t i = j + 1;
                const Real sigma2_new = diffusion_coefficient_squared(T_new, K_grid[i], forward, params);
                const Real sigma2_old = diffusion_coefficient_squared(T_old, K_grid[i], forward, params);

                const Real a_new = theta * dt * sigma2_new / (2.0 * dK * dK);
                const Real a_old = (1.0 - theta) * dt * sigma2_old / (2.0 * dK * dK);

                const Real C_im1 = (j == 0) ? left_boundary_value : C[i - 1];
                const Real C_ip1 = (j == n_interior - 1) ? right_boundary_value : C[i + 1];

                sub[j] = -a_new;
                diag[j] = 1.0 + 2.0 * a_new;
                sup[j] = -a_new;
                rhs[j] = C[i] + a_old * (C_im1 - 2.0 * C[i] + C_ip1);

                if (j == 0)
                    rhs[j] += a_new * left_boundary_value;
                if (j == n_interior - 1)
                    rhs[j] += a_new * right_boundary_value;
            }

            solve_tridiagonal(sub, diag, sup, rhs, x);
            for (std::size_t j = 0; j < n_interior; ++j)
                C[j + 1] = x[j];
        }

        for (std::size_t s = 0; s < strikes.size(); ++s)
            result.call_prices[s] = linear_interp(K_grid, C, strikes[s]);

        return result;
    }

    std::vector<Real> sabr_arbitrage_free_implied_vols(
        Real forward, Real ttm, const SABRParams &params,
        const std::vector<Real> &strikes, Real discount_factor,
        const SABRPDESettings &settings)
    {
        const SABRPDEResult priced = sabr_arbitrage_free_prices(forward, ttm, params, strikes, settings);
        std::vector<Real> ivs(strikes.size());
        for (std::size_t i = 0; i < strikes.size(); ++i)
            ivs[i] = black76_implied_vol(discount_factor * priced.call_prices[i], forward, strikes[i], ttm, discount_factor);
        return ivs;
    }

} // namespace quantModeling
