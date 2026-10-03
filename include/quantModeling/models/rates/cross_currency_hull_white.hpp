#ifndef MODELS_RATES_CROSS_CURRENCY_HULL_WHITE_HPP
#define MODELS_RATES_CROSS_CURRENCY_HULL_WHITE_HPP

#include "quantModeling/models/base.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <array>
#include <string>

namespace quantModeling
{

    /**
     * @brief Two currencies: a Hull-White short rate in each and a lognormal
     *        exchange rate, the three driven by correlated Brownian motions
     *        (blueprint/wp/23-xva.md §14.15, lot X10; Brigo & Mercurio 2006,
     *        §14.3; Piterbarg 2006).
     *
     * Under the domestic risk-neutral measure, with S the price of one unit
     * of foreign currency in domestic currency:
     *
     *   dx_d = (y_d(t) − a_d x_d) dt + σ_d dW_d
     *   dx_f = (y_f(t) − a_f x_f − ρ_fS σ_f σ_S) dt + σ_f dW_f
     *   dS/S = (r_d − r_f) dt + σ_S dW_S
     *
     * x = r − f(0, ·) in each currency, as in HullWhiteCurveModel. The term
     * −ρ_fS σ_f σ_S is the quanto drift: the foreign rate is seen from the
     * domestic measure.
     *
     * **What is simulated is not the spot.** Its drift is the difference of
     * two stochastic rates, whose integral would have to be simulated too.
     * The forward exchange rate to a date T*,
     *
     *   F(t, T*) = S(t) P_f(t, T*) / P_d(t, T*),
     *
     * is a martingale under the domestic T*-forward measure, with a
     * deterministic volatility:
     *
     *   dF/F = σ_S dW_S − σ_f G_f(t, T*) dW_f + σ_d G_d(t, T*) dW_d.
     *
     * The state (x_d, x_f, z), z = ln(F(t, T*) / F(0, T*)), is therefore
     * Gaussian and Markov under that measure: its transitions are exact,
     * and the spot is read back from it, both bonds being closed forms of
     * the rates' states. No time-discretisation error, as in the
     * one-currency engine (ADR-X1).
     */
    class CrossCurrencyHullWhiteModel final : public IModel
    {
      public:
        /// Instantaneous correlations of the three Brownian motions.
        struct Correlations
        {
            Real domestic_foreign = 0.0;
            Real domestic_fx = 0.0;
            Real foreign_fx = 0.0;
        };

        /**
         * @param spot          domestic units per unit of foreign currency.
         * @param fx_volatility σ_S, the volatility of the spot.
         * @throws InvalidInput unless spot > 0, σ_S > 0 and the correlation
         *         matrix is positive definite.
         */
        CrossCurrencyHullWhiteModel(HullWhiteCurveModel domestic, HullWhiteCurveModel foreign,
                                    Real spot, Real fx_volatility, Correlations correlations);

        const HullWhiteCurveModel &domestic() const { return domestic_; }
        const HullWhiteCurveModel &foreign() const { return foreign_; }
        Real spot() const { return spot_; }
        Real fx_volatility() const { return fx_volatility_; }
        const Correlations &correlations() const { return rho_; }

        /// F(0, T) = S(0) P_f(0, T) / P_d(0, T): covered interest parity.
        Real forward_fx(Time T) const;

        /// The spot at t from the state: F(0, T*) e^z P_d(t, T*) / P_f(t, T*).
        Real spot_at(Time t, Time numeraire_maturity, Real x_domestic, Real x_foreign, Real z) const;

        /**
         * @brief The Black volatility of an FX option expiring at T: the
         *        root of the average variance of F(·, T) over [0, T]. Above
         *        σ_S as soon as the rates move: the forward carries the
         *        volatility of both bonds. What σ_S is calibrated through.
         */
        Real fx_implied_volatility(Time T) const;

        /**
         * @brief The exact Gaussian transition of (x_d, x_f, z) from s to t
         *        under the domestic T*-forward measure:
         *
         *   x_d(t) = decay[0] x_d(s) + drift[0] + ξ_0
         *   x_f(t) = decay[1] x_f(s) + drift[1] + ξ_1
         *   z(t)   =            z(s) + drift[2] + ξ_2
         *
         * ξ = L Z, Z three independent standard normals, L lower triangular
         * (`cholesky`, row by row). The first row is the one-currency
         * model's own transition: the domestic rate moves exactly as it does
         * alone.
         */
        struct Transition
        {
            std::array<Real, 2> decay;
            std::array<Real, 3> drift;
            std::array<std::array<Real, 3>, 3> cholesky;
        };
        Transition transition(Time s, Time t, Time numeraire_maturity) const;

        std::string model_name() const noexcept override { return "CrossCurrencyHullWhiteModel"; }

      private:
        /// σ_F(u)² for the forward to T: the squared norm of its volatility.
        Real forward_variance_rate(Time u, Time T) const;

        HullWhiteCurveModel domestic_;
        HullWhiteCurveModel foreign_;
        Real spot_;
        Real fx_volatility_;
        Correlations rho_;
    };

    /**
     * @brief The spot volatility σ_S for which the model's Black volatility
     *        at `expiry` is the market's (fx_implied_volatility). The Black
     *        variance is a quadratic in σ_S — σ_S² T, a cross term through
     *        the correlations of the spot with each rate, and what the two
     *        bonds add — solved exactly.
     *
     * Only the mean reversions and volatilities of the two rate models are
     * read, not their curves.
     *
     * @throws InvalidInput when no positive σ_S gives that volatility: the
     *         rates alone already move the forward more than the market
     *         says.
     */
    Real calibrated_fx_volatility(const HullWhiteCurveModel &domestic, const HullWhiteCurveModel &foreign,
                                  const CrossCurrencyHullWhiteModel::Correlations &correlations,
                                  Time expiry, Real market_volatility);

} // namespace quantModeling

#endif
