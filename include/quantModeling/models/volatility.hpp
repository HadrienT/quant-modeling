#ifndef QUANT_MODELING_MODELS_VOLATILITY_HPP
#define QUANT_MODELING_MODELS_VOLATILITY_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/path_steps.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  IVolatility — abstract volatility surface interface
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Abstract interface for a volatility surface σ(S, t).
     *
     * All MC time-step loops call vol.value(S_current, t_current) at each step,
     * so the engine code is identical for flat-vol (Black-Scholes) and local-vol
     * (Dupire) models.  Analytic and tree engines that only need a scalar sigma
     * can still call ILocalVolModel::vol_sigma() for backward compatibility.
     */
    struct IVolatility
    {
        virtual ~IVolatility() = default;

        /**
         * @brief Local volatility σ(S, t) at spot S and time t (years).
         *
         * Implementations must be noexcept and fast — this is called inside the
         * Monte-Carlo inner loop.
         */
        virtual Real value(Real S, Real t) const noexcept = 0;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  FlatVol — constant (Black-Scholes) volatility
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Constant volatility: σ(S, t) = σ  for all S, t.
     *
     * Used by BlackScholesModel.  value() ignores both arguments.
     */
    struct FlatVol final : public IVolatility
    {
        explicit FlatVol(Real sigma) noexcept
            : sigma_(sigma) {}

        Real value(Real /*S*/, Real /*t*/) const noexcept override
        {
            return sigma_;
        }

        /// Raw scalar access for backward-compatible analytic formulas.
        Real sigma() const noexcept { return sigma_; }

      private:
        Real sigma_;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  GridLocalVol — local-vol surface on a (K × T) grid, C¹ in the strike
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Local-vol surface stored on a rectangular grid: a cubic Hermite
     *        in the strike (C¹, mc::GridView::strike_hermite), linear in
     *        time, flat extrapolation at the boundaries.
     *
     * Grid layout:  sigma_loc[i * n_T + j]  corresponds to (K_grid[i], T_grid[j]).
     * K-major ordering matches the flat array produced by the Dupire calibration
     * pipeline in dupire.py.
     *
     * A vol_shift additive offset enables fast FD vega bumps without copying the
     * underlying grid data — see shifted().
     */
    struct GridLocalVol final : public IVolatility
    {
        /**
         * @param K_grid    Strictly increasing strike / spot grid (size n_K ≥ 2).
         * @param T_grid    Strictly increasing time grid (size n_T ≥ 2).
         * @param sigma_loc Local-vol values, K-major: sigma_loc[i*n_T + j]
         *                  corresponds to (K_grid[i], T_grid[j]).
         * @param vol_shift Additive parallel shift applied to every returned value.
         *                  Default 0.  Use shifted(dv) to create bumped copies.
         */
        GridLocalVol(std::vector<Real> K_grid,
                     std::vector<Real> T_grid,
                     std::vector<Real> sigma_loc,
                     Real vol_shift = 0.0)
            : K_grid_(std::move(K_grid)), T_grid_(std::move(T_grid)), sigma_loc_(std::move(sigma_loc)), vol_shift_(vol_shift)
        {
            if (K_grid_.size() < 2 || T_grid_.size() < 2)
                throw std::invalid_argument(
                    "GridLocalVol: grids must have at least 2 nodes each");
            if (sigma_loc_.size() != K_grid_.size() * T_grid_.size())
                throw std::invalid_argument(
                    "GridLocalVol: sigma_loc size mismatch "
                    "(expected n_K * n_T = " +
                    std::to_string(K_grid_.size() * T_grid_.size()) +
                    ", got " + std::to_string(sigma_loc_.size()) + ")");
        }

        // ── IVolatility interface ────────────────────────────────────────────────

        Real value(Real S, Real t) const noexcept override
        {
            // mc::local_vol_bilinear's interpolation (C¹ in the strike since
            // 2026-11-16, linear in time), the shift added before the floor.
            mc::GridView<Real> g;
            g.K = K_grid_.data();
            g.nK = static_cast<int>(K_grid_.size());
            g.T_grid = T_grid_.data();
            g.nT = static_cast<int>(T_grid_.size());
            g.v = sigma_loc_.data();
            int idx[4], j0, j1;
            Real w[4], dw[4];
            g.strike_hermite(S, idx, w, dw);
            t = std::clamp(t, T_grid_.front(), T_grid_.back());
            mc::bracket(g.T_grid, g.nT, t, j0, j1);
            const Real T0 = T_grid_[static_cast<std::size_t>(j0)];
            const Real dT = T_grid_[static_cast<std::size_t>(j1)] - T0;
            const Real wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;
            Real sigma = 0.0;
            for (int k = 0; k < 4; ++k)
                sigma = sigma + w[k] * ((1.0 - wT) * g.at(idx[k], j0) + wT * g.at(idx[k], j1));
            return std::max(sigma + vol_shift_, Real{1e-6});
        }

        // ── Accessors ────────────────────────────────────────────────────────────

        const std::vector<Real> &K_grid() const noexcept { return K_grid_; }
        const std::vector<Real> &T_grid() const noexcept { return T_grid_; }
        const std::vector<Real> &sigma_loc() const noexcept { return sigma_loc_; }
        Real vol_shift() const noexcept { return vol_shift_; }

        /**
         * @brief Construct a parallel-shifted copy of this surface.
         *
         * Example — FD vega bump in a barrier engine:
         * @code
         *   const GridLocalVol vol_u = grid_vol.shifted(+dv);
         *   const GridLocalVol vol_d = grid_vol.shifted(-dv);
         * @endcode
         * Grid data is shared by value — cheap for small surfaces.
         */
        GridLocalVol shifted(Real dv) const
        {
            return GridLocalVol(K_grid_, T_grid_, sigma_loc_, vol_shift_ + dv);
        }

      private:
        std::vector<Real> K_grid_;
        std::vector<Real> T_grid_;
        std::vector<Real> sigma_loc_;
        Real vol_shift_;
    };

} // namespace quantModeling

#endif // QUANT_MODELING_MODELS_VOLATILITY_HPP
