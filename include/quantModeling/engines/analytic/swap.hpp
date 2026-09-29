#ifndef ENGINE_ANALYTIC_SWAP_HPP
#define ENGINE_ANALYTIC_SWAP_HPP

#include "quantModeling/instruments/rates/swap.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/equity/sabr.hpp"

namespace quantModeling
{

    /**
     * @brief Multi-curve market for a single-currency swap: discount on the
     *        OIS curve, project the floating index on its own curve
     *        (blueprint/wp/21-rates.md §1). Non-owning.
     *
     * Before 2008 one curve did both; since, a collateralised swap is
     * discounted at the collateral rate (OIS) while a 3M index fixes above
     * it by a basis that the single-curve set-up cannot hold.
     */
    struct MultiCurve
    {
        const DiscountCurve &discount;
        const DiscountCurve &projection;
    };

    /// Simple forward rate of `curve` over [start, end] with day-count
    /// fraction `accrual`: (P(start) / P(end) - 1) / accrual.
    Real forward_rate(const DiscountCurve &curve, Time start, Time end, Real accrual);

    /// A swap's legs per its notional, before the payer sign.
    struct SwapValuation
    {
        Real fixed_leg = 0.0;    ///< fixed_rate · annuity · notional
        Real floating_leg = 0.0; ///< Σ (F_j + spread) τ_j P_d(pay_j) · notional
        Real annuity = 0.0;      ///< Σ δ_i P_d(pay_i), per unit notional
        Real par_rate = 0.0;     ///< the fixed rate that makes the swap worth zero
        Real npv = 0.0;          ///< floating − fixed for a payer, the opposite for a receiver
    };

    SwapValuation value_swap(const InterestRateSwap &swap, const MultiCurve &curves);

    /**
     * @brief Black (1976) price of a swaption per unit of annuity-weighted
     *        notional: `annuity · Black(forward, strike, vol, expiry)`, a
     *        call on the swap rate for a payer. A positive `shift` prices
     *        the shifted-lognormal model (forward + shift lognormal), the
     *        market convention since rates went negative.
     */
    Real black_swaption(bool payer, Real forward, Real strike, Time expiry, Real vol, Real annuity,
                        Real shift = 0.0);

    /**
     * @brief Bachelier (normal) price: the swap rate is Gaussian with
     *        absolute volatility `normal_vol` (in rate units: 0.01 = 100bp).
     *        The quote convention of swaption desks since low and negative
     *        rates made the lognormal meaningless.
     */
    Real bachelier_swaption(bool payer, Real forward, Real strike, Time expiry, Real normal_vol,
                            Real annuity);

    /// Vega of bachelier_swaption: annuity · √T · φ(d).
    Real bachelier_vega(Real forward, Real strike, Time expiry, Real normal_vol, Real annuity);

    /**
     * @brief The normal vol that reproduces `price` under Bachelier.
     *        Safeguarded Newton on the (strictly increasing) price in σ,
     *        bisection when a step leaves the bracket. NaN when the price is
     *        below intrinsic or the iteration does not converge — never a
     *        silent guess.
     */
    Real bachelier_implied_vol(bool payer, Real price, Real forward, Real strike, Time expiry,
                               Real annuity);

    /**
     * @brief A swaption priced off a (shifted) SABR smile: Hagan's 2002
     *        lognormal vol at (forward + shift, strike + shift), fed to
     *        black_swaption with the same shift.
     */
    Real sabr_swaption(bool payer, Real forward, Real strike, Time expiry, const SABRParams &params,
                       Real annuity, Real shift = 0.0);

    /// Forward swap rate and annuity of a swaption's underlying, on its curves.
    struct SwaptionForward
    {
        Real forward;
        Real annuity; ///< per unit notional
    };
    SwaptionForward swaption_forward(const Swaption &swaption, const MultiCurve &curves);

} // namespace quantModeling

#endif
