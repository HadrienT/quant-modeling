#include "quantModeling/engines/analytic/swap.hpp"

#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    Real forward_rate(const DiscountCurve &curve, Time start, Time end, Real accrual)
    {
        if (!(accrual > 0.0) || !(end > start))
            throw InvalidInput("forward_rate: need end > start and a positive accrual");
        return (curve.discount(start) / curve.discount(end) - 1.0) / accrual;
    }

    SwapValuation value_swap(const InterestRateSwap &swap, const MultiCurve &curves)
    {
        if (swap.fixed_leg.empty() || swap.floating_leg.empty())
            throw InvalidInput("value_swap: both legs need at least one coupon");
        SwapValuation v;
        for (const CouponPeriod &c : swap.fixed_leg)
            v.annuity += c.accrual * curves.discount.discount(c.payment);
        Real floating = 0.0;
        for (const CouponPeriod &c : swap.floating_leg)
        {
            const Real F = forward_rate(curves.projection, c.start, c.end, c.accrual);
            floating += (F + swap.spread) * c.accrual * curves.discount.discount(c.payment);
        }
        v.par_rate = floating / v.annuity;
        v.fixed_leg = swap.fixed_rate * v.annuity * swap.notional;
        v.floating_leg = floating * swap.notional;
        const Real payer_value = v.floating_leg - v.fixed_leg;
        v.npv = swap.payer ? payer_value : -payer_value;
        return v;
    }

    Real black_swaption(bool payer, Real forward, Real strike, Time expiry, Real vol, Real annuity,
                        Real shift)
    {
        const Real F = forward + shift, K = strike + shift;
        if (!(F > 0.0) || !(K > 0.0))
            throw InvalidInput("black_swaption: forward and strike must be above -shift");
        if (!(expiry > 0.0) || !(vol > 0.0))
        {
            const Real intrinsic = payer ? std::max(F - K, 0.0) : std::max(K - F, 0.0);
            return annuity * intrinsic;
        }
        const Real sd = vol * std::sqrt(expiry);
        const Real d1 = (std::log(F / K) + 0.5 * sd * sd) / sd;
        const Real d2 = d1 - sd;
        const Real call = F * norm_cdf(d1) - K * norm_cdf(d2);
        return annuity * (payer ? call : call - (F - K));
    }

    Real bachelier_swaption(bool payer, Real forward, Real strike, Time expiry, Real normal_vol,
                            Real annuity)
    {
        const Real w = payer ? 1.0 : -1.0;
        if (!(expiry > 0.0) || !(normal_vol > 0.0))
            return annuity * std::max(w * (forward - strike), 0.0);
        const Real sd = normal_vol * std::sqrt(expiry);
        const Real d = (forward - strike) / sd;
        return annuity * (w * (forward - strike) * norm_cdf(w * d) + sd * norm_pdf(d));
    }

    Real bachelier_vega(Real forward, Real strike, Time expiry, Real normal_vol, Real annuity)
    {
        if (!(expiry > 0.0) || !(normal_vol > 0.0))
            return 0.0;
        const Real sd = normal_vol * std::sqrt(expiry);
        return annuity * std::sqrt(expiry) * norm_pdf((forward - strike) / sd);
    }

    Real bachelier_implied_vol(bool payer, Real price, Real forward, Real strike, Time expiry,
                               Real annuity)
    {
        const Real nan = std::numeric_limits<Real>::quiet_NaN();
        if (!(expiry > 0.0) || !(annuity > 0.0))
            return nan;
        const Real intrinsic = annuity * std::max((payer ? 1.0 : -1.0) * (forward - strike), 0.0);
        // Invert the time value, A (sd φ(d) − |F − K| Φ(−|F − K| / sd)): the
        // same for payer and receiver, and free of the cancellation the
        // in-the-money formula carries.
        const Real target = price - intrinsic;
        if (!(target > 0.0))
            return nan;
        const Real m = std::abs(forward - strike), sqrtT = std::sqrt(expiry);
        const auto time_value = [&](Real sigma)
        {
            const Real sd = sigma * sqrtT;
            return annuity * (sd * norm_pdf(m / sd) - m * norm_cdf(-m / sd));
        };
        // σ → 0 has no time value; σ = 1 (10 000bp) is past any real quote.
        Real lo = 0.0, hi = 1.0;
        if (time_value(hi) < target)
            return nan;
        // ATM start: time value = A σ √(T / 2π).
        Real sigma = std::clamp(target / (annuity * sqrtT / std::sqrt(2.0 * M_PI)), 1e-8, hi);
        for (int iter = 0; iter < 200; ++iter)
        {
            const Real diff = time_value(sigma) - target;
            if (diff == 0.0)
                return sigma;
            (diff > 0.0 ? hi : lo) = sigma;
            const Real vega = annuity * sqrtT * norm_pdf(m / (sigma * sqrtT));
            Real next = vega > 0.0 ? sigma - diff / vega : 0.5 * (lo + hi);
            if (!(next > lo && next < hi))
                next = 0.5 * (lo + hi);
            if (std::abs(next - sigma) <= 1e-14 * sigma)
                return next;
            sigma = next;
        }
        return hi - lo <= 1e-10 * hi ? sigma : nan;
    }

    Real sabr_swaption(bool payer, Real forward, Real strike, Time expiry, const SABRParams &params,
                       Real annuity, Real shift)
    {
        const Real vol = sabr_implied_vol(forward + shift, strike + shift, expiry, params);
        return black_swaption(payer, forward, strike, expiry, vol, annuity, shift);
    }

    SwaptionForward swaption_forward(const Swaption &swaption, const MultiCurve &curves)
    {
        if (swaption.swap.start() < swaption.expiry - 1e-10)
            throw InvalidInput("swaption: the underlying swap must start on or after the expiry");
        const SwapValuation v = value_swap(swaption.swap, curves);
        return {v.par_rate, v.annuity};
    }

} // namespace quantModeling
