#ifndef MODELS_RATES_HULL_WHITE_CURVE_HPP
#define MODELS_RATES_HULL_WHITE_CURVE_HPP

#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/base.hpp"

#include <string>

namespace quantModeling
{

    /**
     * @brief Hull-White one-factor model fitted exactly to today's discount
     *        curve, with a deterministic basis to a projection curve
     *        (blueprint/wp/21-rates.md §3).
     *
     *   dr = (θ(t) − a r) dt + σ dW,  θ(t) chosen so that P(0, T) is the
     *   market curve's for every T.
     *
     * Written, as in Andersen & Piterbarg (2010, §10.1), in the state
     * variable x(t) = r(t) − f(0, t), which starts at 0 and never needs
     * θ(t) or the instantaneous forward explicitly:
     *
     *   dx = (y(t) − a x) dt + σ dW,   y(t) = σ² (1 − e^{−2at}) / (2a),
     *   P(t, T | x) = P(0, T) / P(0, t) · exp(−G(t, T) x − ½ G(t, T)² y(t)),
     *   G(t, T) = (1 − e^{−a(T−t)}) / a.
     *
     * Multi-curve: the projection curve keeps a deterministic multiplicative
     * spread to the discount curve, the standard way to carry the basis into
     * a one-factor model (Mercurio 2009). A floating coupon on [s, e] is
     * then worth β P(t, s) − P(t, e) at t ≤ s, β = (1 + τF(0)) P_d(0,e)/P_d(0,s).
     *
     * The discount curve is the OIS curve: this is the collateralised
     * short rate.
     */
    class HullWhiteCurveModel final : public IModel
    {
      public:
        /// @throws InvalidInput unless σ > 0 and |a| ≥ 1e-6 (the formulas
        ///         divide by a; a = 0 is the Ho-Lee limit, not supported).
        HullWhiteCurveModel(Real mean_reversion, Real sigma, DiscountCurve discount,
                            DiscountCurve projection);

        /// Single-curve: the projection curve is the discount curve.
        HullWhiteCurveModel(Real mean_reversion, Real sigma, DiscountCurve discount);

        Real mean_reversion() const { return a_; }
        Real sigma() const { return sigma_; }
        const DiscountCurve &discount() const { return discount_; }
        const DiscountCurve &projection() const { return projection_; }

        /// G(t, T) = (1 − e^{−a(T−t)}) / a.
        Real G(Time t, Time T) const;
        /// y(t) = Var[x(t)] = σ² (1 − e^{−2at}) / (2a).
        Real y(Time t) const;

        /// P(t, T) given the state x(t).
        Real zcb(Time t, Time T, Real x) const;

        /// The Gaussian transition x(s) → x(t) under the T-forward measure
        /// (numeraire P(·, T), T ≥ t):
        ///   x(t) = decay · x(s) + drift + √variance · Z.
        struct Transition
        {
            Real decay;
            Real drift;
            Real variance;
        };
        Transition transition(Time s, Time t, Time numeraire_maturity) const;

        std::string model_name() const noexcept override { return "HullWhiteCurveModel"; }

      private:
        Real a_;
        Real sigma_;
        DiscountCurve discount_;
        DiscountCurve projection_;
    };

} // namespace quantModeling

#endif
