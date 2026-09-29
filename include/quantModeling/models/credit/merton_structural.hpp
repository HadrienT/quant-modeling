#ifndef MODELS_CREDIT_MERTON_STRUCTURAL_HPP
#define MODELS_CREDIT_MERTON_STRUCTURAL_HPP

#include "quantModeling/core/types.hpp"

namespace quantModeling
{

    /**
     * @brief Merton's (1974) structural model of a firm: assets V follow a
     *        geometric Brownian motion with volatility σ_V, the firm owes a
     *        single zero-coupon debt of face D at T, and defaults at T if
     *        V_T < D. Equity is then a call on the assets struck at D, and
     *        the debt is riskless debt minus a put.
     *
     * Everything here is risk-neutral (drift r): the default probability is
     * the Q-probability that prices the debt, not the real-world one Moody's
     * KMV maps to an "expected default frequency" (Crosbie & Bohn 2003).
     *
     * Known limitation, and the reason it is shown next to market spreads
     * rather than instead of them: with a continuous asset path and default
     * only at T, short-dated spreads tend to zero, far below observed ones
     * (Jones, Mason & Rosenfeld 1984; Eom, Helwege & Huang 2004).
     */
    struct MertonFirm
    {
        Real asset_value; ///< V_0
        Real asset_vol;   ///< σ_V, annualised
        Real debt_face;   ///< D, the default point
        Real rate;        ///< continuously compounded riskless rate

        void validate() const;

        /// d2 = (ln(V/D) + (r - σ²/2) T) / (σ √T): the distance to default,
        /// in standard deviations of ln V_T.
        Real distance_to_default(Time T) const;
        /// Q(V_T < D) = N(-d2).
        Real default_probability(Time T) const;
        /// E = V N(d1) - D e^{-rT} N(d2).
        Real equity_value(Time T) const;
        /// σ_E = N(d1) V σ_V / E (Itô on E(V)).
        Real equity_vol(Time T) const;
        /// B = V N(-d1) + D e^{-rT} N(d2) = V - E.
        Real debt_value(Time T) const;
        /// -ln(B / (D e^{-rT})) / T: the yield spread of the risky debt.
        Real credit_spread(Time T) const;
        /// E_Q[V_T | V_T < D] / D = V e^{rT} N(-d1) / (D N(-d2)): the
        /// recovery rate on default that the model implies.
        Real expected_recovery(Time T) const;
    };

    struct MertonCalibration
    {
        MertonFirm firm;
        int iterations = 0;
        bool converged = false;
        /// max(|E_model - E| / E, |σ_E model - σ_E| / σ_E) at the solution.
        Real relative_residual = 0.0;
    };

    /**
     * @brief Recover the unobservable (V_0, σ_V) from the observable equity
     *        value E and equity volatility σ_E — the two equations
     *          E = C(V, σ_V; D, r, T),   σ_E E = N(d1) V σ_V
     *        solved jointly by Newton's method with an analytic Jacobian and
     *        a step that keeps V and σ_V positive (Jones, Mason & Rosenfeld
     *        1984; Crosbie & Bohn 2003). Start: V = E + D e^{-rT},
     *        σ_V = σ_E E / V.
     *
     * @param horizon the debt maturity T of the calibration (1 year is the
     *        KMV convention).
     * @throws InvalidInput on non-positive inputs.
     */
    MertonCalibration calibrate_merton(Real equity_value, Real equity_vol, Real debt_face,
                                       Real rate, Time horizon = 1.0);

} // namespace quantModeling

#endif
