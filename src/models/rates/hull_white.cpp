#include "quantModeling/models/rates/hull_white.hpp"

#include <cmath>

namespace quantModeling
{

    HullWhiteModel::HullWhiteModel(Real a, Real sigma, Real r0, Real theta)
        : a_(a), sigma_(sigma), r0_(r0), theta_(theta)
    {
        if (a_ <= 0.0)
            throw InvalidInput("HullWhiteModel: mean reversion a must be > 0");
        if (sigma_ <= 0.0)
            throw InvalidInput("HullWhiteModel: volatility sigma must be > 0");
    }

    // ── B(τ) = (1 − e^{−aτ}) / a  (same as Vasicek) ────────────────────────

    Real HullWhiteModel::B(Real tau) const
    {
        if (tau <= 0.0)
            return 0.0;
        return (1.0 - std::exp(-a_ * tau)) / a_;
    }

    // ── ln A(τ):  uses b = θ/a ──────────────────────────────────────────────

    Real HullWhiteModel::lnA(Real tau) const
    {
        const Real b = theta_ / a_;
        const Real Bval = B(tau);
        const Real term1 = (Bval - tau) * (a_ * a_ * b - 0.5 * sigma_ * sigma_) / (a_ * a_);
        const Real term2 = -sigma_ * sigma_ * Bval * Bval / (4.0 * a_);
        return term1 + term2;
    }

    // ── P(0, T) ──────────────────────────────────────────────────────────────

    Real HullWhiteModel::zcb_price(Time T) const
    {
        return zcb_price(0.0, T, r0_);
    }

    // ── P(t, T | r_t) ───────────────────────────────────────────────────────

    Real HullWhiteModel::zcb_price(Time t, Time T, Real r_t) const
    {
        if (T <= t)
            return 1.0;
        const Real tau = T - t;
        return std::exp(lnA(tau) - B(tau) * r_t);
    }

    // ── Bond option (same algebra as Vasicek with b ← θ/a) ──────────────────

    Real HullWhiteModel::bond_option_price(bool is_call, Real K,
                                           Time T_option, Time T_bond) const
    {
        if (T_option <= 0.0)
            throw InvalidInput("HullWhiteModel::bond_option_price: T_option must be > 0");
        if (T_bond <= T_option)
            throw InvalidInput("HullWhiteModel::bond_option_price: T_bond must be > T_option");
        if (K <= 0.0)
            throw InvalidInput("HullWhiteModel::bond_option_price: strike K must be > 0");

        const Real P0S = zcb_price(T_bond);
        const Real P0T = zcb_price(T_option);

        const Real sigma_p = (sigma_ / a_) *
                             (1.0 - std::exp(-a_ * (T_bond - T_option))) *
                             std::sqrt((1.0 - std::exp(-2.0 * a_ * T_option)) / (2.0 * a_));

        const Real h = (1.0 / sigma_p) * std::log(P0S / (K * P0T)) + 0.5 * sigma_p;

        if (is_call)
            return P0S * norm_cdf(h) - K * P0T * norm_cdf(h - sigma_p);
        else
            return K * P0T * norm_cdf(-h + sigma_p) - P0S * norm_cdf(-h);
    }

    // ── Euler step ───────────────────────────────────────────────────────────
    //
    //   r(t+dt) = r(t) + (θ − a·r(t)) dt + σ dW

    Real HullWhiteModel::euler_step(Real r, Real /*t*/, Real dt, Real dW) const
    {
        return r + (theta_ - a_ * r) * dt + sigma_ * dW;
    }

} // namespace quantModeling
