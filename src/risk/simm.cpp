#include "quantModeling/risk/simm.hpp"

#include "quantModeling/utils/inverse_normal.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling::simm
{
    namespace
    {
        constexpr Real kMillion = 1e6;

        /// The two vertices around `years` and the weight of the first.
        struct Bracket
        {
            std::size_t lower;
            std::size_t upper;
            Real weight_lower;
        };

        Bracket bracket(Time years)
        {
            if (!(years > vertex_years.front()))
                return {0, 0, 1.0};
            if (!(years < vertex_years.back()))
                return {vertices - 1, vertices - 1, 1.0};
            const std::size_t upper = static_cast<std::size_t>(
                std::upper_bound(vertex_years.begin(), vertex_years.end(), years) -
                vertex_years.begin());
            const Time a = vertex_years[upper - 1], b = vertex_years[upper];
            return {upper - 1, upper, (b - years) / (b - a)};
        }

        void spread(std::array<Real, vertices> &on, Time years, Real amount)
        {
            const Bracket b = bracket(years);
            on[b.lower] += b.weight_lower * amount;
            if (b.upper != b.lower)
                on[b.upper] += (1.0 - b.weight_lower) * amount;
        }

        Real sum(const std::array<Real, vertices> &v)
        {
            Real s = 0.0;
            for (const Real x : v)
                s += x;
            return s;
        }

        /// sqrt(Σ_k Σ_l c_kl v_k v_l), c the correlation or its square.
        Real aggregate(const std::array<Real, vertices> &v, bool squared_correlation)
        {
            Real total = 0.0;
            for (std::size_t k = 0; k < vertices; ++k)
                for (std::size_t l = 0; l < vertices; ++l)
                {
                    const Real c = correlation[k][l];
                    total += (squared_correlation && k != l ? c * c : c) * v[k] * v[l];
                }
            // The matrix is positive definite; rounding can leave -1e-20.
            return std::sqrt(std::max(total, 0.0));
        }

        void require_finite(const std::array<Real, vertices> &v)
        {
            for (const Real x : v)
                if (!std::isfinite(x))
                    throw InvalidInput("SIMM: a sensitivity is not finite");
        }
    } // namespace

    CurrencyParameters regular_well_traded()
    {
        return {{107, 101, 90, 69, 68, 69, 66, 61, 60, 58, 58, 66}, 220.0 * kMillion, 3800.0 * kMillion};
    }

    CurrencyParameters regular_less_traded()
    {
        return {{107, 101, 90, 69, 68, 69, 66, 61, 60, 58, 58, 66}, 110.0 * kMillion, 520.0 * kMillion};
    }

    CurrencyParameters low_volatility()
    {
        return {{15, 18, 12, 11, 15, 21, 23, 25, 29, 27, 26, 28}, 370.0 * kMillion, 1100.0 * kMillion};
    }

    CurrencyParameters high_volatility()
    {
        return {{167, 102, 79, 82, 90, 93, 92, 88, 88, 98, 101, 96}, 71.0 * kMillion, 160.0 * kMillion};
    }

    Real scaling_function(Time expiry_years)
    {
        if (!(expiry_years > 0.0))
            return 0.5;
        return 0.5 * std::min(1.0, 14.0 / (365.0 * expiry_years));
    }

    void Sensitivities::add_delta(Time years, Real pv01)
    {
        spread(delta, years, pv01);
    }

    void Sensitivities::add_vega(Time expiry_years, Real vega_times_vol)
    {
        spread(vega, expiry_years, vega_times_vol);
        spread(curvature, expiry_years, scaling_function(expiry_years) * vega_times_vol);
    }

    Margin interest_rate_margin(const Sensitivities &s, const CurrencyParameters &currency)
    {
        require_finite(s.delta);
        require_finite(s.vega);
        require_finite(s.curvature);
        Margin margin;

        // Delta (§7): one currency, one sub-curve, so the margin is K.
        const Real cr =
            std::max(1.0, std::sqrt(std::abs(sum(s.delta)) / currency.delta_concentration_threshold));
        std::array<Real, vertices> weighted{};
        for (std::size_t k = 0; k < vertices; ++k)
            weighted[k] = currency.risk_weight[k] * s.delta[k] * cr;
        margin.delta = aggregate(weighted, false);

        // Vega (§10): the inner adjustment factors are 1 for interest rates.
        const Real vcr =
            std::max(1.0, std::sqrt(std::abs(sum(s.vega)) / currency.vega_concentration_threshold));
        for (std::size_t k = 0; k < vertices; ++k)
            weighted[k] = vega_risk_weight * s.vega[k] * vcr;
        margin.vega = aggregate(weighted, false);

        // Curvature (§11): squared correlations, then the scale factor
        // HVR^-2 of the interest rate class.
        Real total = 0.0, absolute = 0.0;
        for (const Real cvr : s.curvature)
        {
            total += cvr;
            absolute += std::abs(cvr);
        }
        if (absolute > 0.0)
        {
            const Real theta = std::min(total / absolute, 0.0);
            const Real quantile = inverse_normal_cdf(0.995);
            const Real lambda = (quantile * quantile - 1.0) * (1.0 + theta) - theta;
            margin.curvature = std::max(total + lambda * aggregate(s.curvature, true), 0.0) /
                               (historical_volatility_ratio * historical_volatility_ratio);
        }
        return margin;
    }

} // namespace quantModeling::simm
