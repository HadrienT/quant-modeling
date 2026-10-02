#ifndef QM_RISK_REGULATORY_SA_CVA_HPP
#define QM_RISK_REGULATORY_SA_CVA_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/risk/regulatory/ba_cva.hpp"

#include <array>
#include <cstddef>

/**
 * @file sa_cva.hpp
 * @brief The standardised approach for CVA risk (Basel framework, MAR50.42 to
 *        MAR50.77), for what a netting set of interest-rate trades in one of
 *        the specified currencies needs: the interest-rate delta and vega and
 *        the counterparty credit spread delta
 *        (blueprint/wp/23-xva.md §14.13, lot X8).
 *
 * The capital is computed from the **sensitivities of the regulatory CVA** —
 * the unilateral CVA, the bank's own default left out (MAR50.3) — to
 * prescribed risk factors. The text allows them to be computed by adjoint
 * differentiation (MAR50.47, FAQ1). For each risk class,
 *
 *   WS_k = RW_k s_k,
 *   K_b  = sqrt(Σ_k WS_k² + Σ_k Σ_{l≠k} ρ_kl WS_k WS_l + R Σ_k (WS_k^hdg)²)   (MAR50.53),
 *   K    = m_CVA sqrt(Σ_b K_b² + Σ_b Σ_{c≠b} γ_bc S_b S_c),
 *
 * and the classes add up (MAR50.43, 50.45). One currency and one
 * counterparty give one bucket per class: K = m_CVA K_b. No hedge is
 * recognised here, so the hedging-disallowance term is zero.
 *
 * Every figure below is read from the text; the paragraph is quoted next to
 * it.
 */

namespace quantModeling::sa_cva
{

    /// The tenors of the risk-free yield, in years (MAR50.56).
    inline constexpr std::array<Time, 5> interest_rate_tenors{1.0, 2.0, 5.0, 10.0, 30.0};
    /// Their risk weights, Table 3 of MAR50.56 (specified currencies).
    inline constexpr std::array<Real, 5> interest_rate_risk_weights{0.0111, 0.0093, 0.0074, 0.0074, 0.0074};
    /// Their correlations, Table 4 of MAR50.56.
    inline constexpr std::array<std::array<Real, 5>, 5> interest_rate_correlations{{
        {1.00, 0.91, 0.72, 0.55, 0.31},
        {0.91, 1.00, 0.87, 0.72, 0.45},
        {0.72, 0.87, 1.00, 0.91, 0.68},
        {0.55, 0.72, 0.91, 1.00, 0.83},
        {0.31, 0.45, 0.68, 0.83, 1.00},
    }};
    /// The risk weight of the interest-rate volatilities (MAR50.58).
    inline constexpr Real interest_rate_vega_risk_weight = 1.0;

    /// The tenors of the counterparty's credit spread (MAR50.65).
    inline constexpr std::array<Time, 5> credit_spread_tenors{0.5, 1.0, 3.0, 5.0, 10.0};
    /// Correlation between two tenors of the same name (MAR50.65).
    inline constexpr Real credit_spread_tenor_correlation = 0.90;

    /// m_CVA (MAR50.41).
    inline constexpr Real default_multiplier = 1.0;

    /// Risk weight of a counterparty's credit spread, Table 7 of MAR50.65:
    /// by sector, and by investment grade against high yield or not rated.
    Real credit_spread_risk_weight(ba_cva::Sector sector, ba_cva::CreditQuality quality);

    /**
     * @brief The sensitivities of the regulatory CVA of one counterparty, as
     *        the text defines them: the change of the CVA for a small change
     *        of the factor, divided by the size of the change.
     */
    struct Sensitivities
    {
        /// To the risk-free yield of each tenor (per unit of rate: the change
        /// for 1 bp divided by 0.0001, MAR50.56).
        std::array<Real, 5> interest_rate_delta{};
        /// To a simultaneous relative shift of all interest-rate volatilities
        /// (the change for 1 % divided by 0.01, MAR50.58).
        Real interest_rate_vega = 0.0;
        /// To the counterparty's credit spread of each tenor (per unit of
        /// spread, MAR50.65).
        std::array<Real, 5> credit_spread_delta{};
        ba_cva::Sector sector = ba_cva::Sector::Other;
        ba_cva::CreditQuality quality = ba_cva::CreditQuality::HighYieldOrNotRated;
    };

    struct Capital
    {
        Real interest_rate_delta = 0.0;
        Real interest_rate_vega = 0.0;
        Real credit_spread_delta = 0.0;

        Real total() const { return interest_rate_delta + interest_rate_vega + credit_spread_delta; }
    };

    /// @throws InvalidInput on a sensitivity that is not finite or a
    ///         multiplier below one.
    Capital capital(const Sensitivities &s, Real multiplier = default_multiplier);

    /**
     * @brief Spreads sensitivities known at arbitrary tenors over the
     *        prescribed ones: a shift of the prescribed tenor T_k moves every
     *        point of the curve by the weight a linear interpolation between
     *        the prescribed tenors gives it (flat beyond the first and the
     *        last), so the sensitivity to T_k is the sum of the sensitivities
     *        weighted the same way.
     */
    template <std::size_t N>
    std::array<Real, N> to_tenors(const std::array<Time, N> &prescribed, const Time *tenors,
                                  const Real *sensitivities, std::size_t count)
    {
        std::array<Real, N> out{};
        for (std::size_t i = 0; i < count; ++i)
        {
            const Time t = tenors[i];
            if (t <= prescribed.front())
                out.front() += sensitivities[i];
            else if (t >= prescribed.back())
                out.back() += sensitivities[i];
            else
                for (std::size_t k = 1; k < N; ++k)
                    if (t <= prescribed[k])
                    {
                        const Real w = (t - prescribed[k - 1]) / (prescribed[k] - prescribed[k - 1]);
                        out[k - 1] += (1.0 - w) * sensitivities[i];
                        out[k] += w * sensitivities[i];
                        break;
                    }
        }
        return out;
    }

} // namespace quantModeling::sa_cva

#endif // QM_RISK_REGULATORY_SA_CVA_HPP
