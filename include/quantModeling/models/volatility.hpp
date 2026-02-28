#ifndef QUANT_MODELING_MODELS_VOLATILITY_HPP
#define QUANT_MODELING_MODELS_VOLATILITY_HPP

#include "quantModeling/core/types.hpp"

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
        explicit FlatVol(Real sigma) noexcept : sigma_(sigma) {}

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
    //  GridLocalVol — bilinear-interpolated local-vol surface on a (K × T) grid
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Local-vol surface stored on a rectangular grid with bilinear
     *        interpolation and flat extrapolation at the boundaries.
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
            const int nK = static_cast<int>(K_grid_.size());
            const int nT = static_cast<int>(T_grid_.size());

            // Clamp to grid bounds (flat extrapolation)
            S = std::clamp(S, K_grid_.front(), K_grid_.back());
            t = std::clamp(t, T_grid_.front(), T_grid_.back());

            // Locate bracketing index pair — K dimension
            auto it_K = std::lower_bound(K_grid_.begin(), K_grid_.end(), S);
            int i1 = static_cast<int>(it_K - K_grid_.begin());
            if (i1 >= nK)
                i1 = nK - 1;
            int i0 = (i1 > 0) ? i1 - 1 : 0;
            if (i0 == i1)
            {
                if (i1 > 0)
                    i0 = i1 - 1;
                else
                    i1 = 1;
            }

            // Locate bracketing index pair — T dimension
            auto it_T = std::lower_bound(T_grid_.begin(), T_grid_.end(), t);
            int j1 = static_cast<int>(it_T - T_grid_.begin());
            if (j1 >= nT)
                j1 = nT - 1;
            int j0 = (j1 > 0) ? j1 - 1 : 0;
            if (j0 == j1)
            {
                if (j1 > 0)
                    j0 = j1 - 1;
                else
                    j1 = 1;
            }

            const Real K0 = K_grid_[i0], K1 = K_grid_[i1];
            const Real T0 = T_grid_[j0], T1 = T_grid_[j1];

            const Real dK = K1 - K0;
            const Real dT = T1 - T0;
            const Real wK = (dK > 1e-12) ? (S - K0) / dK : 0.0;
            const Real wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;

            const auto cell = [&](int i, int j) -> Real
            {
                return sigma_loc_[static_cast<std::size_t>(i) *
                                      static_cast<std::size_t>(nT) +
                                  static_cast<std::size_t>(j)];
            };

            const Real sigma =
                (1.0 - wK) * (1.0 - wT) * cell(i0, j0) + wK * (1.0 - wT) * cell(i1, j0) + (1.0 - wK) * wT * cell(i0, j1) + wK * wT * cell(i1, j1) + vol_shift_;

            return std::max(sigma, Real{1e-6});
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
