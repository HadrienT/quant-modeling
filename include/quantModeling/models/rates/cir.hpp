#ifndef MODELS_RATES_CIR_HPP
#define MODELS_RATES_CIR_HPP

#include "quantModeling/models/rates/short_rate_model.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    /**
     * @brief Cox-Ingersoll-Ross (1985) short-rate model.
     *
     *   dr(t) = a (b − r(t)) dt + σ √r(t) dW(t)
     *
     * Square-root diffusion — rates are always non-negative when the
     * Feller condition 2ab ≥ σ² is satisfied.
     *
     * Closed-form for zero-coupon bond prices P(t, T | r).
     * Bond option pricing uses Monte Carlo (chi-squared analytic
     * formula not implemented).
     */
    struct CIRModel final : public IShortRateModel
    {
        /**
         * @param a     Mean-reversion speed (a > 0).
         * @param b     Long-term mean rate (b > 0).
         * @param sigma Volatility (σ > 0).
         * @param r0    Initial short rate r(0) ≥ 0.
         * @throws InvalidInput if Feller condition 2ab ≥ σ² is violated.
         */
        CIRModel(Real a, Real b, Real sigma, Real r0);

        // ── IShortRateModel interface ────────────────────────────────────────

        Real r0() const override { return r0_; }
        Real mean_reversion() const override { return a_; }
        Real long_term_rate() const override { return b_; }
        Real volatility() const override { return sigma_; }

        Real zcb_price(Time T) const override;
        Real zcb_price(Time t, Time T, Real r_t) const override;

        /// CIR Euler step with reflection to keep r ≥ 0.
        Real euler_step(Real r, Real t, Real dt, Real dW) const override;

        std::string model_name() const noexcept override { return "CIRModel"; }

        // ── Helpers (public for testing) ─────────────────────────────────────

        /// γ = √(a² + 2σ²)
        Real gamma() const;

        /// B(τ) for CIR
        Real B(Real tau) const;

        /// A(τ) for CIR
        Real A(Real tau) const;

        /// Check Feller condition 2ab ≥ σ²
        bool feller_satisfied() const;

    private:
        Real a_, b_, sigma_, r0_;
    };

} // namespace quantModeling

#endif
