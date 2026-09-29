#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <cmath>

namespace quantModeling
{

    HullWhiteCurveModel::HullWhiteCurveModel(Real mean_reversion, Real sigma, DiscountCurve discount,
                                             DiscountCurve projection)
        : a_(mean_reversion), sigma_(sigma), discount_(std::move(discount)),
          projection_(std::move(projection))
    {
        if (!(sigma_ > 0.0))
            throw InvalidInput("HullWhiteCurveModel: sigma must be > 0");
        if (!(std::abs(a_) >= 1e-6))
            throw InvalidInput("HullWhiteCurveModel: |mean reversion| must be >= 1e-6");
    }

    HullWhiteCurveModel::HullWhiteCurveModel(Real mean_reversion, Real sigma, DiscountCurve discount)
        : HullWhiteCurveModel(mean_reversion, sigma, discount, discount)
    {
    }

    Real HullWhiteCurveModel::G(Time t, Time T) const
    {
        return -std::expm1(-a_ * (T - t)) / a_;
    }

    Real HullWhiteCurveModel::y(Time t) const
    {
        return sigma_ * sigma_ * -std::expm1(-2.0 * a_ * t) / (2.0 * a_);
    }

    Real HullWhiteCurveModel::zcb(Time t, Time T, Real x) const
    {
        const Real g = G(t, T);
        return discount_.discount(T) / discount_.discount(t) * std::exp(-g * x - 0.5 * g * g * y(t));
    }

    HullWhiteCurveModel::Transition HullWhiteCurveModel::transition(Time s, Time t,
                                                                    Time numeraire_maturity) const
    {
        const Real a = a_, s2 = sigma_ * sigma_;
        const Real d = t - s;
        // ∫_s^t e^{−a(t−u)} y(u) du and ∫_s^t e^{−a(t−u)} G(u, T) du, in
        // expm1 form so that a small a loses digits only in proportion to 1/(a d).
        const Real I1 = s2 / (2.0 * a * a) * (-std::expm1(-a * d) - std::exp(-2.0 * a * t) * std::expm1(a * d));
        const Real I2 = (-std::expm1(-a * d) + 0.5 * std::exp(-a * (numeraire_maturity - t)) * std::expm1(-2.0 * a * d)) /
                        (a * a);
        return {std::exp(-a * d), I1 - s2 * I2, s2 * -std::expm1(-2.0 * a * d) / (2.0 * a)};
    }

} // namespace quantModeling
