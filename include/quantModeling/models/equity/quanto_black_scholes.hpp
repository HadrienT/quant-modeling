#ifndef EQUITY_QUANTO_BLACK_SCHOLES_HPP
#define EQUITY_QUANTO_BLACK_SCHOLES_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/models/volatility.hpp"
#include <string>

namespace quantModeling
{

    /**
     * @brief Black-Scholes for an asset quoted in a FOREIGN currency whose
     *        payoff is paid in the DOMESTIC currency at a conversion rate
     *        fixed in advance (a quanto) — Reiner, "Quanto mechanics",
     *        Risk 5(3), 1992; Hull, Options, Futures and Other Derivatives,
     *        ch. "Quantos".
     *
     * With S the asset (foreign currency), X the FX rate in domestic per
     * foreign, σ_S, σ_X their volatilities and ρ = corr(dS/S, dX/X), the
     * asset's drift under the DOMESTIC risk-neutral measure is
     *
     *     r_f − q − ρ σ_S σ_X ,
     *
     * the quanto adjustment: holding the foreign asset in domestic terms
     * carries the FX exposure the fixed conversion rate removes. The payoff
     * is discounted at r_d. This model therefore presents the vanilla
     * engines with rate r_d and the dividend yield that yields that drift,
     *
     *     q_eff = r_d − r_f + q + ρ σ_S σ_X ,
     *
     * and a vanilla option on S with notional X̄ (the fixed rate) priced on
     * it IS the quanto (Garman-Kohlhagen is the same device with q_eff = r_f).
     * No new instrument or engine: the model is the whole difference.
     *
     * Caveat for sensitivities: q_eff depends on r_d and σ_S, so an engine's
     * rho or vega, which hold q_eff fixed, are not the quanto's — see
     * pricers/adapters/equity_quanto.cpp, which corrects both.
     */
    struct QuantoBlackScholesModel final : public ILocalVolModel
    {
        Real s0_, r_d_, r_f_, q_, sigma_s_, sigma_x_, rho_;

        QuantoBlackScholesModel(Real s0, Real r_d, Real r_f, Real q, Real sigma_s,
                                Real sigma_x, Real rho)
            : s0_(s0), r_d_(r_d), r_f_(r_f), q_(q), sigma_s_(sigma_s), sigma_x_(sigma_x), rho_(rho), flat_vol_(sigma_s), disc_curve_(r_d)
        {
        }

        /// ρ σ_S σ_X: how much the quanto lowers the asset's drift.
        Real quanto_adjustment() const { return rho_ * sigma_s_ * sigma_x_; }

        Real spot0() const override { return s0_; }
        Real rate_r() const override { return r_d_; }
        Real yield_q() const override { return r_d_ - r_f_ + q_ + quanto_adjustment(); }
        Real vol_sigma() const override { return sigma_s_; }
        const IVolatility &vol() const override { return flat_vol_; }
        const DiscountCurve &discount_curve() const override { return disc_curve_; }

        std::string model_name() const noexcept override
        {
            return "QuantoBlackScholesModel";
        }

      private:
        FlatVol flat_vol_;
        DiscountCurve disc_curve_;
    };

} // namespace quantModeling

#endif
