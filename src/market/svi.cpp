#include "quantModeling/market/svi.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    namespace
    {
        Real discriminant(Real x, Real sigma) noexcept
        {
            return std::sqrt(x * x + sigma * sigma);
        }
    } // namespace

    Real svi_total_variance(Real k, const SVIParams &p) noexcept
    {
        const Real x = k - p.m;
        return p.a + p.b * (p.rho * x + discriminant(x, p.sigma));
    }

    Real svi_total_variance_dk(Real k, const SVIParams &p) noexcept
    {
        const Real x = k - p.m;
        const Real D = discriminant(x, p.sigma);
        return p.b * (p.rho + x / D);
    }

    Real svi_total_variance_dk2(Real k, const SVIParams &p) noexcept
    {
        const Real x = k - p.m;
        const Real D = discriminant(x, p.sigma);
        return p.b * p.sigma * p.sigma / (D * D * D);
    }

    Real svi_implied_vol(Real k, Real T, const SVIParams &p) noexcept
    {
        const Real w = svi_total_variance(k, p);
        return std::sqrt(std::max(w, Real(0.0)) / T);
    }

    Real svi_density_g(Real k, const SVIParams &p) noexcept
    {
        const Real w = svi_total_variance(k, p);
        if (w <= 0.0)
            return -1.0; // degenerate slice: treated as an arbitrage violation

        const Real wp = svi_total_variance_dk(k, p);
        const Real wpp = svi_total_variance_dk2(k, p);

        const Real term1 = 1.0 - (k * wp) / (2.0 * w);
        return term1 * term1 - (wp * wp / 4.0) * (1.0 / w + 0.25) + wpp / 2.0;
    }

    bool svi_satisfies_necessary_conditions(const SVIParams &p) noexcept
    {
        if (p.b < 0.0)
            return false;
        if (!(p.rho > -1.0 && p.rho < 1.0))
            return false;
        if (p.sigma <= 0.0)
            return false;

        const Real w_min = p.a + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
        return w_min >= 0.0;
    }

    bool svi_is_butterfly_arbitrage_free(const SVIParams &p, Real k_min, Real k_max,
                                         std::size_t n_grid) noexcept
    {
        if (!svi_satisfies_necessary_conditions(p))
            return false;
        if (n_grid < 2 || !(k_max > k_min))
            return false;

        for (std::size_t i = 0; i < n_grid; ++i)
        {
            const Real t = static_cast<Real>(i) / static_cast<Real>(n_grid - 1);
            const Real k = k_min + t * (k_max - k_min);
            if (svi_density_g(k, p) < 0.0)
                return false;
        }
        return true;
    }

} // namespace quantModeling
