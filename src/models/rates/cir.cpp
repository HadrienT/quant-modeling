#include "quantModeling/models/rates/cir.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    CIRModel::CIRModel(Real a, Real b, Real sigma, Real r0)
        : a_(a), b_(b), sigma_(sigma), r0_(r0)
    {
        if (a_ <= 0.0)
            throw InvalidInput("CIRModel: mean reversion a must be > 0");
        if (b_ <= 0.0)
            throw InvalidInput("CIRModel: long-term rate b must be > 0");
        if (sigma_ <= 0.0)
            throw InvalidInput("CIRModel: volatility sigma must be > 0");
        if (r0_ < 0.0)
            throw InvalidInput("CIRModel: initial rate r0 must be >= 0");
        if (!feller_satisfied())
            throw InvalidInput("CIRModel: Feller condition 2ab >= sigma^2 violated");
    }

    bool CIRModel::feller_satisfied() const
    {
        return 2.0 * a_ * b_ >= sigma_ * sigma_;
    }

    // ── γ = √(a² + 2σ²) ─────────────────────────────────────────────────────

    Real CIRModel::gamma() const
    {
        return std::sqrt(a_ * a_ + 2.0 * sigma_ * sigma_);
    }

    // ── B(τ) = 2(e^{γτ} − 1) / ((γ + a)(e^{γτ} − 1) + 2γ) ─────────────────

    Real CIRModel::B(Real tau) const
    {
        if (tau <= 0.0)
            return 0.0;
        const Real g = gamma();
        const Real egt = std::exp(g * tau);
        return 2.0 * (egt - 1.0) / ((g + a_) * (egt - 1.0) + 2.0 * g);
    }

    // ── A(τ) = [2γ e^{(a+γ)τ/2} / ((γ+a)(e^{γτ}−1) + 2γ)]^{2ab/σ²} ───────

    Real CIRModel::A(Real tau) const
    {
        if (tau <= 0.0)
            return 1.0;
        const Real g = gamma();
        const Real egt = std::exp(g * tau);
        const Real denom = (g + a_) * (egt - 1.0) + 2.0 * g;
        const Real numer = 2.0 * g * std::exp((a_ + g) * tau * 0.5);
        const Real exponent = 2.0 * a_ * b_ / (sigma_ * sigma_);
        return std::pow(numer / denom, exponent);
    }

    // ── P(0, T) ──────────────────────────────────────────────────────────────

    Real CIRModel::zcb_price(Time T) const
    {
        return zcb_price(0.0, T, r0_);
    }

    // ── P(t, T | r_t) = A(τ) · exp(−B(τ) · r_t) ────────────────────────────

    Real CIRModel::zcb_price(Time t, Time T, Real r_t) const
    {
        if (T <= t)
            return 1.0;
        const Real tau = T - t;
        return A(tau) * std::exp(-B(tau) * r_t);
    }

    // ── Euler step with reflection ───────────────────────────────────────────
    //
    //   r(t+dt) = |r(t) + a(b − r(t)) dt + σ √r(t) dW|
    //   Reflection keeps r ≥ 0.

    Real CIRModel::euler_step(Real r, Real /*t*/, Real dt, Real dW) const
    {
        const Real r_pos = std::max(r, 0.0);
        const Real next = r_pos + a_ * (b_ - r_pos) * dt + sigma_ * std::sqrt(r_pos) * dW;
        return std::abs(next); // reflection at zero
    }

} // namespace quantModeling
