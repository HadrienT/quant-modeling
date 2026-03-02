#ifndef MODELS_RATES_HULL_WHITE_HPP
#define MODELS_RATES_HULL_WHITE_HPP

#include "quantModeling/models/rates/short_rate_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    /**
     * @brief Hull-White (1990) one-factor short-rate model.
     *
     *   dr(t) = (θ(t) − a r(t)) dt + σ dW(t)
     *
     * This implementation uses a **constant** θ, making it equivalent to
     * the Vasicek model with b = θ / a.  The abstraction is kept separate
     * because Hull-White is conventionally calibrated differently and can
     * be extended to time-dependent θ(t) for exact term-structure fitting.
     *
     * Closed-form for ZCB prices and European bond options (same algebra
     * as Vasicek with the mapping b ← θ / a).
     */
    struct HullWhiteModel final : public IShortRateModel
    {
        /**
         * @param a     Mean-reversion speed (a > 0).
         * @param sigma Volatility (σ > 0).
         * @param r0    Initial short rate r(0).
         * @param theta Constant drift speed (θ).  Long-term rate = θ / a.
         */
        HullWhiteModel(Real a, Real sigma, Real r0, Real theta);

        // ── IShortRateModel interface ────────────────────────────────────────

        Real r0() const override { return r0_; }
        Real mean_reversion() const override { return a_; }
        Real long_term_rate() const override { return theta_ / a_; }
        Real volatility() const override { return sigma_; }

        Real zcb_price(Time T) const override;
        Real zcb_price(Time t, Time T, Real r_t) const override;

        /**
         * @brief Analytic European option on a ZCB (same formula as Vasicek).
         */
        Real bond_option_price(bool is_call, Real K,
                               Time T_option, Time T_bond) const override;

        Real euler_step(Real r, Real t, Real dt, Real dW) const override;

        std::string model_name() const noexcept override { return "HullWhiteModel"; }

        // ── Helpers ──────────────────────────────────────────────────────────

        Real theta() const { return theta_; }

        /// B(τ) = (1 − e^{−aτ}) / a
        Real B(Real tau) const;

        /// ln A(τ)  (uses b = θ/a)
        Real lnA(Real tau) const;

    private:
        Real a_, sigma_, r0_, theta_;
    };

} // namespace quantModeling

#endif
