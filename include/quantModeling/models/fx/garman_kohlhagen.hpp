#ifndef MODELS_FX_GARMAN_KOHLHAGEN_HPP
#define MODELS_FX_GARMAN_KOHLHAGEN_HPP

#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/models/volatility.hpp"

namespace quantModeling
{

    /**
     * @brief Garman-Kohlhagen FX model.
     *
     * Maps onto the ILocalVolModel interface:
     *   spot0()    → S0  (spot FX rate, domestic per foreign)
     *   rate_r()   → r_d (domestic risk-free rate)
     *   yield_q()  → r_f (foreign risk-free rate — acts as dividend yield)
     *   vol_sigma()→ σ   (FX volatility)
     */
    struct GarmanKohlhagenModel final : public ILocalVolModel
    {
        Real s0_;    ///< spot FX rate
        Real r_d_;   ///< domestic rate
        Real r_f_;   ///< foreign rate
        Real sigma_; ///< FX volatility
        FlatVol flat_vol_;

        GarmanKohlhagenModel(Real s0, Real r_d, Real r_f, Real sigma)
            : s0_(s0), r_d_(r_d), r_f_(r_f), sigma_(sigma), flat_vol_(sigma), disc_curve_(r_d) {}

        Real spot0() const override { return s0_; }
        Real rate_r() const override { return r_d_; }
        Real yield_q() const override { return r_f_; }
        Real vol_sigma() const override { return sigma_; }
        const IVolatility &vol() const override { return flat_vol_; }
        const DiscountCurve &discount_curve() const override { return disc_curve_; }
        std::string model_name() const noexcept override { return "GarmanKohlhagenModel"; }

    private:
        DiscountCurve disc_curve_;
    };

} // namespace quantModeling

#endif
