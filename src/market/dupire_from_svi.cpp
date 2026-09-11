#include "quantModeling/market/dupire_from_svi.hpp"

#include "quantModeling/market/svi.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    Real svi_surface_local_variance(const SVISurface &surface, Real k, Real T,
                                    const DupireFromSVIParams &params) noexcept
    {
        constexpr Real nan = std::numeric_limits<Real>::quiet_NaN();

        const Real w = surface.total_variance(k, T);
        if (w <= 0.0)
            return nan;

        const Real dwdT = surface.total_variance_dT(k, T);
        if (dwdT < 0.0)
            return nan; // calendar arbitrage at this point

        const Real wp = surface.total_variance_dk(k, T);
        const Real wpp = surface.total_variance_dk2(k, T);
        const Real denom = svi_density_g_from_variance(k, w, wp, wpp);

        if (std::fabs(denom) < params.denom_floor)
            return nan;

        const Real local_var = dwdT / denom;
        if (local_var < 0.0)
            return nan;

        return std::clamp(local_var, params.min_local_var, params.max_local_var);
    }

    namespace
    {

        /// Nearest-neighbour fill for NaN cells, matching dupire.py's
        /// fallback: ring search outward in grid-index space, good enough for
        /// the sparse, localised gaps this formula produces in practice (a
        /// pervasively unstable calibration should have failed its own
        /// report long before a grid is built from it).
        void fill_gaps_nearest_neighbour(std::vector<Real> &grid, std::size_t n_strikes, std::size_t n_maturities)
        {
            const std::vector<Real> source = grid;
            const auto valid_at = [&](std::size_t i, std::size_t j)
            { return !std::isnan(source[i * n_maturities + j]); };

            for (std::size_t i = 0; i < n_strikes; ++i)
            {
                for (std::size_t j = 0; j < n_maturities; ++j)
                {
                    if (!std::isnan(source[i * n_maturities + j]))
                        continue;

                    bool found = false;
                    const std::size_t max_radius = n_strikes + n_maturities;
                    for (std::size_t radius = 1; radius <= max_radius && !found; ++radius)
                    {
                        for (std::size_t di = 0; di <= radius && !found; ++di)
                        {
                            const std::size_t dj = radius - di;
                            const long long signs[2] = {1, -1};
                            for (const long long si : signs)
                            {
                                for (const long long sj : signs)
                                {
                                    const long long ci = static_cast<long long>(i) + si * static_cast<long long>(di);
                                    const long long cj = static_cast<long long>(j) + sj * static_cast<long long>(dj);
                                    if (ci < 0 || cj < 0)
                                        continue;
                                    const auto ui = static_cast<std::size_t>(ci);
                                    const auto uj = static_cast<std::size_t>(cj);
                                    if (ui >= n_strikes || uj >= n_maturities || !valid_at(ui, uj))
                                        continue;
                                    grid[i * n_maturities + j] = source[ui * n_maturities + uj];
                                    found = true;
                                    break;
                                }
                                if (found)
                                    break;
                            }
                        }
                    }
                }
            }
        }

    } // namespace

    GridLocalVol build_local_vol_grid(const SVISurface &surface, Real spot, Real rate, Real dividend,
                                      Real k_min, Real k_max,
                                      std::size_t n_strikes, std::size_t n_maturities,
                                      const DupireFromSVIParams &params)
    {
        std::vector<Real> T_grid(n_maturities);
        for (std::size_t j = 0; j < n_maturities; ++j)
        {
            const Real t = (n_maturities > 1) ? static_cast<Real>(j) / static_cast<Real>(n_maturities - 1) : 0.0;
            T_grid[j] = surface.ttm_min() + t * (surface.ttm_max() - surface.ttm_min());
        }

        // Strike grid: fixed log-moneyness band, converted to strikes at the
        // mid-maturity forward -- same convention as
        // api/app/local_vol/dupire.py's calibrate_dupire.
        const Real T_mid = 0.5 * (surface.ttm_min() + surface.ttm_max());
        const Real F_mid = spot * std::exp((rate - dividend) * T_mid);
        std::vector<Real> K_grid(n_strikes);
        for (std::size_t i = 0; i < n_strikes; ++i)
        {
            const Real t = (n_strikes > 1) ? static_cast<Real>(i) / static_cast<Real>(n_strikes - 1) : 0.0;
            K_grid[i] = F_mid * std::exp(k_min + t * (k_max - k_min));
        }

        std::vector<Real> loc_var(n_strikes * n_maturities);
        for (std::size_t i = 0; i < n_strikes; ++i)
        {
            for (std::size_t j = 0; j < n_maturities; ++j)
            {
                const Real T = T_grid[j];
                const Real F_T = spot * std::exp((rate - dividend) * T);
                const Real k = std::log(K_grid[i] / F_T);
                loc_var[i * n_maturities + j] = svi_surface_local_variance(surface, k, T, params);
            }
        }

        fill_gaps_nearest_neighbour(loc_var, n_strikes, n_maturities);

        std::vector<Real> sigma_loc(loc_var.size());
        for (std::size_t idx = 0; idx < loc_var.size(); ++idx)
        {
            const Real v = std::isnan(loc_var[idx]) ? params.min_local_var : loc_var[idx];
            sigma_loc[idx] = std::sqrt(std::clamp(v, params.min_local_var, params.max_local_var));
        }

        return GridLocalVol(std::move(K_grid), std::move(T_grid), std::move(sigma_loc));
    }

} // namespace quantModeling
