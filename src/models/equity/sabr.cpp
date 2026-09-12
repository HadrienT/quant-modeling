#include "quantModeling/models/equity/sabr.hpp"

#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    Real sabr_implied_vol(Real forward, Real strike, Real ttm, const SABRParams &p) noexcept
    {
        const Real F = forward, K = strike, T = ttm;
        const Real alpha = p.alpha, beta = p.beta, rho = p.rho, nu = p.nu;
        const Real one_minus_beta = 1.0 - beta;

        const Real log_FK = std::log(F / K);
        const Real FK_pow_half = std::pow(F * K, one_minus_beta / 2.0);

        const Real log_FK2 = log_FK * log_FK;
        const Real series_correction =
            1.0 + (one_minus_beta * one_minus_beta / 24.0) * log_FK2 +
            (one_minus_beta * one_minus_beta * one_minus_beta * one_minus_beta / 1920.0) * log_FK2 * log_FK2;

        const Real prefactor = alpha / (FK_pow_half * series_correction);

        const Real time_correction =
            1.0 + ((one_minus_beta * one_minus_beta / 24.0) * (alpha * alpha) / std::pow(F * K, one_minus_beta) +
                   0.25 * rho * beta * nu * alpha / FK_pow_half + ((2.0 - 3.0 * rho * rho) / 24.0) * nu * nu) *
                      T;

        // z is exactly 0 not only at K == F but also whenever nu == 0 (no
        // vol-of-vol), for every strike -- x(z) is then 0 too, so z/x(z) is a
        // removable 0/0 singularity that a K==F check alone does not catch.
        // x(z) = z - rho*z^2/2 + O(z^3) near 0, so z/x(z) -> 1 + rho*z/2.
        const Real z = (nu / alpha) * FK_pow_half * log_FK;
        Real z_over_x;
        if (std::fabs(z) < 1e-8)
        {
            z_over_x = 1.0 + 0.5 * rho * z;
        }
        else
        {
            const Real sqrt_term = std::sqrt(1.0 - 2.0 * rho * z + z * z);
            const Real x_of_z = std::log((sqrt_term + z - rho) / (1.0 - rho));
            z_over_x = z / x_of_z;
        }

        return prefactor * z_over_x * time_correction;
    }

    Real black76_call_price(Real forward, Real strike, Real ttm, Real vol, Real discount_factor) noexcept
    {
        if (vol <= 0.0 || ttm <= 0.0)
            return discount_factor * std::max(forward - strike, Real(0.0));

        const Real vol_sqrt_t = vol * std::sqrt(ttm);
        const Real d1 = (std::log(forward / strike) + 0.5 * vol * vol * ttm) / vol_sqrt_t;
        const Real d2 = d1 - vol_sqrt_t;
        return discount_factor * (forward * norm_cdf(d1) - strike * norm_cdf(d2));
    }

    Real black76_vega(Real forward, Real strike, Real ttm, Real vol, Real discount_factor) noexcept
    {
        if (vol <= 0.0 || ttm <= 0.0)
            return 0.0;
        const Real vol_sqrt_t = vol * std::sqrt(ttm);
        const Real d1 = (std::log(forward / strike) + 0.5 * vol * vol * ttm) / vol_sqrt_t;
        return discount_factor * forward * std::sqrt(ttm) * norm_pdf(d1);
    }

    Real black76_implied_vol(Real price, Real forward, Real strike, Real ttm, Real discount_factor) noexcept
    {
        constexpr Real nan = std::numeric_limits<Real>::quiet_NaN();
        if (ttm <= 0.0 || forward <= 0.0 || strike <= 0.0 || discount_factor <= 0.0)
            return nan;

        const Real intrinsic = discount_factor * std::max(forward - strike, Real(0.0));
        const Real upper_bound = discount_factor * forward;
        if (!(price > intrinsic && price < upper_bound))
            return nan;

        Real vol = 0.2; // generic starting point; Newton converges fast on a smooth, convex objective here
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            const Real model_price = black76_call_price(forward, strike, ttm, vol, discount_factor);
            const Real diff = model_price - price;
            if (std::fabs(diff) < 1e-10 * discount_factor * forward)
                return vol;

            const Real vega = black76_vega(forward, strike, ttm, vol, discount_factor);
            if (vega < 1e-14)
                break; // flat objective: Newton cannot make progress, fall through to bisection

            const Real next_vol = vol - diff / vega;
            vol = (next_vol > 1e-6 && next_vol < 10.0) ? next_vol : 0.5 * (vol + std::clamp(next_vol, Real(1e-6), Real(10.0)));
        }

        // Newton stalled or overshot: bisection is slow but always converges on
        // this monotone, single-crossing objective.
        Real lo = 1e-6, hi = 10.0;
        if (black76_call_price(forward, strike, ttm, lo, discount_factor) > price ||
            black76_call_price(forward, strike, ttm, hi, discount_factor) < price)
            return nan;

        for (int iteration = 0; iteration < 200; ++iteration)
        {
            const Real mid = 0.5 * (lo + hi);
            const Real model_price = black76_call_price(forward, strike, ttm, mid, discount_factor);
            if (std::fabs(model_price - price) < 1e-10 * discount_factor * forward)
                return mid;
            if (model_price < price)
                lo = mid;
            else
                hi = mid;
        }
        return 0.5 * (lo + hi);
    }

} // namespace quantModeling
