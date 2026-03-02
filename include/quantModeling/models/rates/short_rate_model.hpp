#ifndef MODELS_RATES_SHORT_RATE_MODEL_HPP
#define MODELS_RATES_SHORT_RATE_MODEL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/base.hpp"

namespace quantModeling
{

    /**
     * @brief Abstract interface for one-factor short-rate models.
     *
     * Provides the minimal contract needed by analytic bond-pricing engines,
     * Monte-Carlo path simulators, and tree-based lattice methods.
     *
     * Concrete implementations:
     *   - VasicekModel   dr = a(b − r) dt + σ dW
     *   - CIRModel       dr = a(b − r) dt + σ √r dW
     *   - HullWhiteModel dr = (θ − a·r) dt + σ dW
     */
    struct IShortRateModel : public IModel
    {
        virtual ~IShortRateModel() = default;

        // ── Model parameters ────────────────────────────────────────────────

        /// Initial short rate r(0).
        virtual Real r0() const = 0;

        /// Mean-reversion speed a.
        virtual Real mean_reversion() const = 0;

        /// Long-term mean level b (or θ/a for Hull-White).
        virtual Real long_term_rate() const = 0;

        /// Instantaneous volatility σ.
        virtual Real volatility() const = 0;

        // ── Zero-coupon bond pricing ────────────────────────────────────────

        /// Analytic ZCB price P(0, T) from t = 0.
        virtual Real zcb_price(Time T) const = 0;

        /// Analytic ZCB price P(t, T | r(t)).
        virtual Real zcb_price(Time t, Time T, Real r_t) const = 0;

        // ── Bond option pricing (override in models with closed-form) ───────

        /**
         * @brief European option on a ZCB: call/put on P(T_option, T_bond).
         *
         * Default implementation throws — override in models that have
         * closed-form bond-option formulas (Vasicek, Hull-White).
         */
        virtual Real bond_option_price(bool /*is_call*/, Real /*K*/,
                                       Time /*T_option*/, Time /*T_bond*/) const
        {
            throw UnsupportedInstrument(
                "Analytic bond option pricing not available for " + model_name());
        }

        // ── MC Euler step ───────────────────────────────────────────────────

        /**
         * @brief One Euler-Maruyama step: r(t + dt) given r(t) and dW ~ N(0, √dt).
         *
         * Lets a generic MC engine drive the simulation without knowing
         * the specific SDE.
         */
        virtual Real euler_step(Real r, Real t, Real dt, Real dW) const = 0;
    };

} // namespace quantModeling

#endif
