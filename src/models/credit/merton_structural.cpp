#include "quantModeling/models/credit/merton_structural.hpp"

#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{
    namespace
    {
        struct D12
        {
            Real d1, d2;
        };

        D12 d12(Real V, Real sigma, Real D, Real r, Time T)
        {
            const Real s = sigma * std::sqrt(T);
            const Real d1 = (std::log(V / D) + (r + 0.5 * sigma * sigma) * T) / s;
            return {d1, d1 - s};
        }

        void check_horizon(Time T)
        {
            if (!(T > 0.0))
                throw InvalidInput("Merton: maturity must be > 0");
        }
    } // namespace

    void MertonFirm::validate() const
    {
        if (!(asset_value > 0.0) || !(asset_vol > 0.0) || !(debt_face > 0.0))
            throw InvalidInput("MertonFirm: asset value, asset vol and debt must be > 0");
    }

    Real MertonFirm::distance_to_default(Time T) const
    {
        validate();
        check_horizon(T);
        return d12(asset_value, asset_vol, debt_face, rate, T).d2;
    }

    Real MertonFirm::default_probability(Time T) const
    {
        return norm_cdf(-distance_to_default(T));
    }

    Real MertonFirm::equity_value(Time T) const
    {
        validate();
        check_horizon(T);
        const auto [d1, d2] = d12(asset_value, asset_vol, debt_face, rate, T);
        return asset_value * norm_cdf(d1) - debt_face * std::exp(-rate * T) * norm_cdf(d2);
    }

    Real MertonFirm::equity_vol(Time T) const
    {
        const Real E = equity_value(T);
        const Real d1 = d12(asset_value, asset_vol, debt_face, rate, T).d1;
        return norm_cdf(d1) * asset_value * asset_vol / E;
    }

    Real MertonFirm::debt_value(Time T) const
    {
        validate();
        check_horizon(T);
        const auto [d1, d2] = d12(asset_value, asset_vol, debt_face, rate, T);
        return asset_value * norm_cdf(-d1) + debt_face * std::exp(-rate * T) * norm_cdf(d2);
    }

    Real MertonFirm::credit_spread(Time T) const
    {
        const Real B = debt_value(T);
        // -ln(B / (D e^{-rT})) / T, written so a spread of a fraction of a
        // basis point does not vanish in the subtraction of two logs.
        const Real ratio = B / (debt_face * std::exp(-rate * T));
        return -std::log1p(ratio - 1.0) / T;
    }

    Real MertonFirm::expected_recovery(Time T) const
    {
        validate();
        check_horizon(T);
        const auto [d1, d2] = d12(asset_value, asset_vol, debt_face, rate, T);
        const Real pd = norm_cdf(-d2);
        if (pd <= 0.0)
            return 1.0; // default has no mass: the limit V_T -> D.
        return asset_value * std::exp(rate * T) * norm_cdf(-d1) / (debt_face * pd);
    }

    MertonCalibration calibrate_merton(Real E, Real sigma_E, Real D, Real r, Time T)
    {
        if (!(E > 0.0) || !(sigma_E > 0.0) || !(D > 0.0))
            throw InvalidInput("calibrate_merton: equity value, equity vol and debt must be > 0");
        check_horizon(T);

        const Real sqrtT = std::sqrt(T);
        Real V = E + D * std::exp(-r * T);
        Real sigma = sigma_E * E / V;

        auto residuals = [&](Real v, Real s, Real &f1, Real &f2)
        {
            const auto [d1, d2] = d12(v, s, D, r, T);
            f1 = v * norm_cdf(d1) - D * std::exp(-r * T) * norm_cdf(d2) - E;
            f2 = norm_cdf(d1) * v * s - sigma_E * E;
        };
        auto relative = [&](Real f1, Real f2)
        { return std::max(std::fabs(f1) / E, std::fabs(f2) / (sigma_E * E)); };

        MertonCalibration out;
        Real f1 = 0.0, f2 = 0.0;
        residuals(V, sigma, f1, f2);
        for (out.iterations = 0; out.iterations < 100; ++out.iterations)
        {
            if (relative(f1, f2) < 1e-12)
            {
                out.converged = true;
                break;
            }
            const auto [d1, d2] = d12(V, sigma, D, r, T);
            const Real Nd1 = norm_cdf(d1), nd1 = norm_pdf(d1);
            // ∂E/∂V = N(d1), ∂E/∂σ = V φ(d1) √T (vega),
            // ∂(N(d1)Vσ)/∂V = σ N(d1) + φ(d1)/√T, ∂(N(d1)Vσ)/∂σ = V (N(d1) - φ(d1) d2).
            const Real j11 = Nd1, j12 = V * nd1 * sqrtT;
            const Real j21 = sigma * Nd1 + nd1 / sqrtT, j22 = V * (Nd1 - nd1 * d2);
            const Real det = j11 * j22 - j12 * j21;
            if (!(std::fabs(det) > 0.0))
                break;
            const Real dV = (j22 * f1 - j12 * f2) / det;
            const Real dS = (j11 * f2 - j21 * f1) / det;

            // Halve the step until it keeps V, σ positive and lowers the residual.
            Real step = 1.0;
            const Real before = relative(f1, f2);
            for (int k = 0; k < 40; ++k, step *= 0.5)
            {
                const Real V_new = V - step * dV, s_new = sigma - step * dS;
                if (V_new <= 0.0 || s_new <= 0.0)
                    continue;
                Real g1 = 0.0, g2 = 0.0;
                residuals(V_new, s_new, g1, g2);
                if (relative(g1, g2) < before || k == 39)
                {
                    V = V_new;
                    sigma = s_new;
                    f1 = g1;
                    f2 = g2;
                    break;
                }
            }
        }
        out.firm = MertonFirm{V, sigma, D, r};
        out.relative_residual = relative(f1, f2);
        out.converged = out.converged || out.relative_residual < 1e-10;
        return out;
    }

} // namespace quantModeling
