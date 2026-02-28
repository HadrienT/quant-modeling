#ifndef EQUITY_DUPIRE_HPP
#define EQUITY_DUPIRE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/models/volatility.hpp"

#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Dupire local-volatility model backed by a calibrated σ_loc(K, T) grid.
     *
     * Implements ILocalVolModel so it can be plugged directly into any MC engine
     * that calls m.vol().value(S, t) per time step (barrier, lookback, asian, …).
     *
     * The surface is stored inside a GridLocalVol member; the model owns the data.
     * vol_sigma() returns a representative ATM proxy (σ_loc(S0, 0)) for any analytic
     * or tree engine that only needs a scalar — those engines should not be used
     * with a Dupire model in production, but having a scalar fallback prevents
     * hard failures.
     */
    struct DupireModel final : public ILocalVolModel
    {
        /**
         * @param s0         Initial spot price.
         * @param r          Risk-free rate (continuous, annualised).
         * @param q          Dividend yield (continuous, annualised).
         * @param K_grid     Strictly increasing strike grid (size n_K ≥ 2).
         * @param T_grid     Strictly increasing time grid (size n_T ≥ 2).
         * @param sigma_loc  Flattened σ_loc values, K-major:
         *                   sigma_loc[i * n_T + j] = σ_loc(K_grid[i], T_grid[j]).
         */
        DupireModel(Real s0, Real r, Real q,
                    std::vector<Real> K_grid,
                    std::vector<Real> T_grid,
                    std::vector<Real> sigma_loc)
            : s0_(s0), r_(r), q_(q), surface_(std::move(K_grid), std::move(T_grid), std::move(sigma_loc))
        {
        }

        Real spot0() const override { return s0_; }
        Real rate_r() const override { return r_; }
        Real yield_q() const override { return q_; }

        /// ATM proxy σ_loc(S0, 0) — used only by analytic / tree engines as fallback.
        Real vol_sigma() const override { return surface_.value(s0_, 0.0); }

        /// Full local-vol surface for per-step MC lookup.
        const IVolatility &vol() const override { return surface_; }

        std::string model_name() const noexcept override { return "DupireModel"; }

    private:
        Real s0_, r_, q_;
        GridLocalVol surface_;
    };

} // namespace quantModeling

#endif // EQUITY_DUPIRE_HPP
