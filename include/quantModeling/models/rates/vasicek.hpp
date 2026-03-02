#ifndef MODELS_RATES_VASICEK_HPP
#define MODELS_RATES_VASICEK_HPP

#include "quantModeling/models/rates/short_rate_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{

    /**
     * @brief Vasicek (1977) short-rate model.
     *
     *   dr(t) = a (b − r(t)) dt + σ dW(t)
     *
     * Mean-reverting Gaussian process — rates can go negative.
     *
     * Closed-form available for:
     *   • Zero-coupon bond price  P(t, T | r)
     *   • European bond option    (Jamshidian decomposition)
     */
    struct VasicekModel final : public IShortRateModel
    {
        /**
         * @param a     Mean-reversion speed (a > 0).
         * @param b     Long-term mean rate.
         * @param sigma Volatility (σ > 0).
         * @param r0    Initial short rate r(0).
         */
        VasicekModel(Real a, Real b, Real sigma, Real r0);

        // ── IShortRateModel interface ────────────────────────────────────────

        Real r0() const override { return r0_; }
        Real mean_reversion() const override { return a_; }
        Real long_term_rate() const override { return b_; }
        Real volatility() const override { return sigma_; }

        Real zcb_price(Time T) const override;
        Real zcb_price(Time t, Time T, Real r_t) const override;

        /**
         * @brief Analytic European option on a ZCB.
         *
         * Uses the Jamshidian (1989) formula:
         *   σ_p = (σ/a)(1 − e^{−a(S−T)}) √((1 − e^{−2aT}) / (2a))
         *   Call = P(0,S) N(h) − K P(0,T) N(h − σ_p)
         *   Put  = K P(0,T) N(−h + σ_p) − P(0,S) N(−h)
         */
        Real bond_option_price(bool is_call, Real K,
                               Time T_option, Time T_bond) const override;

        Real euler_step(Real r, Real t, Real dt, Real dW) const override;

        std::string model_name() const noexcept override { return "VasicekModel"; }

        // ── Helpers (public for testing) ─────────────────────────────────────

        /// B(τ) = (1 − e^{−aτ}) / a
        Real B(Real tau) const;

        /// ln A(τ)
        Real lnA(Real tau) const;

    private:
        Real a_, b_, sigma_, r0_;
    };

} // namespace quantModeling

#endif
