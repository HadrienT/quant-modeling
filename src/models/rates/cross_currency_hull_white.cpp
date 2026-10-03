#include "quantModeling/models/rates/cross_currency_hull_white.hpp"

#include <cmath>
#include <numbers>
#include <utility>

namespace quantModeling
{
    namespace
    {
        constexpr int kNodes = 16;

        /// Gauss-Legendre nodes and weights on [-1, 1], by Newton on the
        /// Legendre polynomial. The integrands here are products of
        /// exponentials over one step of the grid: sixteen nodes integrate
        /// them to the last digits.
        struct GaussLegendre
        {
            std::array<Real, kNodes> node, weight;
            GaussLegendre()
            {
                for (int i = 0; i < kNodes; ++i)
                {
                    Real x = std::cos(std::numbers::pi * (i + 0.75) / (kNodes + 0.5));
                    Real derivative = 0.0;
                    for (int iteration = 0; iteration < 100; ++iteration)
                    {
                        Real p0 = 1.0, p1 = x;
                        for (int k = 2; k <= kNodes; ++k)
                        {
                            const Real p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
                            p0 = p1;
                            p1 = p2;
                        }
                        derivative = kNodes * (x * p1 - p0) / (x * x - 1.0);
                        const Real step = p1 / derivative;
                        x -= step;
                        if (std::abs(step) < 1e-16)
                            break;
                    }
                    node[i] = x;
                    weight[i] = 2.0 / ((1.0 - x * x) * derivative * derivative);
                }
            }
        };

        template <class F>
        Real integrate(Time from, Time to, F f)
        {
            static const GaussLegendre rule;
            const Real half = 0.5 * (to - from), middle = 0.5 * (to + from);
            Real sum = 0.0;
            for (int i = 0; i < kNodes; ++i)
                sum += rule.weight[i] * f(middle + half * rule.node[i]);
            return half * sum;
        }
    } // namespace

    CrossCurrencyHullWhiteModel::CrossCurrencyHullWhiteModel(HullWhiteCurveModel domestic,
                                                             HullWhiteCurveModel foreign, Real spot,
                                                             Real fx_volatility,
                                                             Correlations correlations)
        : domestic_(std::move(domestic)), foreign_(std::move(foreign)), spot_(spot), fx_volatility_(fx_volatility), rho_(correlations)
    {
        if (!(spot_ > 0.0) || !std::isfinite(spot_))
            throw InvalidInput("CrossCurrencyHullWhiteModel: the spot must be finite and > 0");
        if (!(fx_volatility_ > 0.0) || !std::isfinite(fx_volatility_))
            throw InvalidInput("CrossCurrencyHullWhiteModel: the FX volatility must be finite and > 0");
        const Real df = rho_.domestic_foreign, ds = rho_.domestic_fx, fs = rho_.foreign_fx;
        // Positive definite: the leading minors of the correlation matrix.
        const Real determinant = 1.0 + 2.0 * df * ds * fs - df * df - ds * ds - fs * fs;
        if (!(std::abs(df) < 1.0) || !(std::abs(ds) < 1.0) || !(std::abs(fs) < 1.0) ||
            !(determinant > 0.0))
            throw InvalidInput("CrossCurrencyHullWhiteModel: the correlations do not form a "
                               "positive definite matrix");
    }

    Real CrossCurrencyHullWhiteModel::forward_fx(Time T) const
    {
        return spot_ * foreign_.discount().discount(T) / domestic_.discount().discount(T);
    }

    Real CrossCurrencyHullWhiteModel::spot_at(Time t, Time numeraire_maturity, Real x_domestic,
                                              Real x_foreign, Real z) const
    {
        return forward_fx(numeraire_maturity) * std::exp(z) *
               domestic_.zcb(t, numeraire_maturity, x_domestic) /
               foreign_.zcb(t, numeraire_maturity, x_foreign);
    }

    Real CrossCurrencyHullWhiteModel::forward_variance_rate(Time u, Time T) const
    {
        const Real s = fx_volatility_;
        const Real d = domestic_.sigma() * domestic_.G(u, T);
        const Real f = foreign_.sigma() * foreign_.G(u, T);
        return s * s + d * d + f * f + 2.0 * rho_.domestic_fx * s * d - 2.0 * rho_.foreign_fx * s * f -
               2.0 * rho_.domestic_foreign * d * f;
    }

    Real CrossCurrencyHullWhiteModel::fx_implied_volatility(Time T) const
    {
        if (!(T > 0.0))
            throw InvalidInput("CrossCurrencyHullWhiteModel: the expiry must be > 0");
        // In pieces of at most a year: the integrand bends with e^{-a(T-u)}.
        const int pieces = static_cast<int>(std::ceil(T));
        Real variance = 0.0;
        for (int k = 0; k < pieces; ++k)
            variance += integrate(T * k / pieces, T * (k + 1) / pieces,
                                  [this, T](Time u)
                                  { return forward_variance_rate(u, T); });
        return std::sqrt(variance / T);
    }

    CrossCurrencyHullWhiteModel::Transition
    CrossCurrencyHullWhiteModel::transition(Time s, Time t, Time numeraire_maturity) const
    {
        if (!(t > s) || !(s >= 0.0) || !(numeraire_maturity >= t))
            throw InvalidInput("CrossCurrencyHullWhiteModel: need 0 <= s < t <= numeraire maturity");
        const Time T = numeraire_maturity;
        const Real ad = domestic_.mean_reversion(), af = foreign_.mean_reversion();
        const Real sd = domestic_.sigma(), sf = foreign_.sigma(), ss = fx_volatility_;
        const Real rdf = rho_.domestic_foreign, rds = rho_.domestic_fx, rfs = rho_.foreign_fx;

        // The domestic rate: the one-currency model's transition, unchanged.
        const HullWhiteCurveModel::Transition domestic = domestic_.transition(s, t, T);

        Transition out;
        out.decay = {domestic.decay, std::exp(-af * (t - s))};
        // The foreign rate under the domestic T*-forward measure: its own
        // drift y_f, the quanto term, and the change to the forward measure
        // through its correlation with the domestic rate.
        out.drift[0] = domestic.drift;
        out.drift[1] = integrate(s, t,
                                 [&](Time u)
                                 {
                                     return std::exp(-af * (t - u)) *
                                            (foreign_.y(u) - rfs * sf * ss - rdf * sf * sd * domestic_.G(u, T));
                                 });
        const Real variance_z = integrate(s, t,
                                          [this, T](Time u)
                                          { return forward_variance_rate(u, T); });
        // A martingale: the drift of its logarithm is minus half its variance.
        out.drift[2] = -0.5 * variance_z;

        // Covariances of the three Gaussian increments.
        const Real c00 = domestic.variance;
        const Real c11 = sf * sf * -std::expm1(-2.0 * af * (t - s)) / (2.0 * af);
        const Real c01 = rdf * sd * sf *
                         (std::abs(ad + af) > 1e-12 ? -std::expm1(-(ad + af) * (t - s)) / (ad + af)
                                                    : t - s);
        const Real c02 = integrate(s, t,
                                   [&](Time u)
                                   {
                                       return std::exp(-ad * (t - u)) * sd *
                                              (rds * ss - rdf * sf * foreign_.G(u, T) + sd * domestic_.G(u, T));
                                   });
        const Real c12 = integrate(s, t,
                                   [&](Time u)
                                   {
                                       return std::exp(-af * (t - u)) * sf *
                                              (rfs * ss - sf * foreign_.G(u, T) + rdf * sd * domestic_.G(u, T));
                                   });
        const Real c22 = variance_z;

        // Cholesky, first row the domestic rate's own standard deviation.
        const Real l00 = std::sqrt(c00);
        const Real l10 = c01 / l00, l20 = c02 / l00;
        const Real l11 = std::sqrt(c11 - l10 * l10);
        const Real l21 = (c12 - l20 * l10) / l11;
        const Real l22 = std::sqrt(c22 - l20 * l20 - l21 * l21);
        if (!std::isfinite(l11) || !std::isfinite(l22))
            throw InvalidInput("CrossCurrencyHullWhiteModel: the covariance of a step is not positive "
                               "definite");
        out.cholesky = {{{l00, 0.0, 0.0}, {l10, l11, 0.0}, {l20, l21, l22}}};
        return out;
    }

} // namespace quantModeling
