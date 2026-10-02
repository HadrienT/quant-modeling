#ifndef QM_RISK_SIMM_HPP
#define QM_RISK_SIMM_HPP

#include "quantModeling/core/types.hpp"

#include <array>
#include <cstddef>

namespace quantModeling::simm
{

    /**
     * @file simm.hpp
     * @brief ISDA SIMM, the industry's standard initial margin model, for
     *        the **interest rate risk class of one currency** — what a
     *        netting set of swaps and swaptions in one currency needs
     *        (blueprint/wp/23-xva.md §5.3, lot X5b).
     *
     * Every parameter below is that of the ISDA SIMM Methodology,
     * **version 2.8+2512** (calibrated to December 2025, effective 11 July
     * 2026), read from the public document; the paragraph is given with
     * each. ISDA recalibrates them every year: a new version is a change of
     * these tables, not of the code.
     *
     * The margin is built from sensitivities, not from a simulation:
     *
     *   delta      WS_k = RW_k s_k CR,  K = sqrt(Σ_k Σ_l ρ_kl WS_k WS_l)     (§7)
     *   vega       VR_k = VRW (Σ σ ∂V/∂σ)_k VCR,  K likewise                 (§10)
     *   curvature  CVR_k = Σ SF(t) σ ∂V/∂σ,
     *              max(Σ CVR + λ sqrt(Σ Σ ρ² CVR CVR), 0) / HVR²             (§11)
     *
     * and the three add up. s_k is the PV01 at vertex k of the yield curve,
     * ρ the correlation between vertices, CR a concentration factor that is
     * 1 below a threshold.
     *
     * Covered: one currency, one sub-curve (the overnight curve). Not
     * covered: several currencies or sub-curves (§7c, §7d), inflation and
     * cross-currency basis, the other risk classes and the aggregation
     * across them (§6). Using SIMM commercially takes a licence from ISDA.
     */

    /// The vertices of the yield curve (§14): 2w, 1m, 3m, 6m, 1y, 2y, 3y,
    /// 5y, 10y, 15y, 20y, 30y.
    inline constexpr std::size_t vertices = 12;
    /// In years, with the document's convention for curvature (§11a): 12m is
    /// 365 days, 1m is 365/12 days, two weeks 14 days.
    inline constexpr std::array<Time, vertices> vertex_years = {
        14.0 / 365.0, 1.0 / 12.0, 0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 10.0, 15.0, 20.0, 30.0};

    /// Correlations between vertices (§36).
    inline constexpr std::array<std::array<Real, vertices>, vertices> correlation = {{
        {1.00, 0.74, 0.65, 0.54, 0.40, 0.29, 0.25, 0.22, 0.17, 0.16, 0.14, 0.14},
        {0.74, 1.00, 0.85, 0.72, 0.50, 0.36, 0.30, 0.25, 0.20, 0.16, 0.14, 0.14},
        {0.65, 0.85, 1.00, 0.90, 0.69, 0.53, 0.46, 0.40, 0.34, 0.27, 0.25, 0.25},
        {0.54, 0.72, 0.90, 1.00, 0.86, 0.73, 0.65, 0.58, 0.52, 0.47, 0.44, 0.42},
        {0.40, 0.50, 0.69, 0.86, 1.00, 0.94, 0.87, 0.81, 0.73, 0.69, 0.64, 0.63},
        {0.29, 0.36, 0.53, 0.73, 0.94, 1.00, 0.97, 0.92, 0.86, 0.82, 0.77, 0.76},
        {0.25, 0.30, 0.46, 0.65, 0.87, 0.97, 1.00, 0.97, 0.91, 0.87, 0.82, 0.81},
        {0.22, 0.25, 0.40, 0.58, 0.81, 0.92, 0.97, 1.00, 0.96, 0.93, 0.89, 0.88},
        {0.17, 0.20, 0.34, 0.52, 0.73, 0.86, 0.91, 0.96, 1.00, 0.98, 0.95, 0.95},
        {0.16, 0.16, 0.27, 0.47, 0.69, 0.82, 0.87, 0.93, 0.98, 1.00, 0.98, 0.97},
        {0.14, 0.14, 0.25, 0.44, 0.64, 0.77, 0.82, 0.89, 0.95, 0.98, 1.00, 0.98},
        {0.14, 0.14, 0.25, 0.42, 0.63, 0.76, 0.81, 0.88, 0.95, 0.97, 0.98, 1.00},
    }};

    /// VRW, the vega risk weight of the interest rate risk class (§35).
    inline constexpr Real vega_risk_weight = 0.20;
    /// HVR, the historical volatility ratio of the class (§34): the
    /// curvature margin is divided by its square (§11).
    inline constexpr Real historical_volatility_ratio = 0.74;

    /// What depends on the currency: its risk weights (§33) and its
    /// concentration thresholds (§74, §81).
    struct CurrencyParameters
    {
        /// RW_k, per vertex: the move of the yield, in basis points, the
        /// margin covers.
        std::array<Real, vertices> risk_weight;
        /// T_b: net PV01 (currency per basis point) above which the delta
        /// is scaled up. The document gives it in USD millions per bp.
        Real delta_concentration_threshold;
        /// VT_b: net vega exposure (currency) above which the vega is
        /// scaled up.
        Real vega_concentration_threshold;
    };

    /// USD, EUR, GBP (Table 1; "regular volatility, well-traded").
    CurrencyParameters regular_well_traded();
    /// AUD, CAD, CHF, DKK, HKD, KRW, NOK, NZD, SEK, SGD, TWD (Table 1).
    CurrencyParameters regular_less_traded();
    /// JPY (Table 2).
    CurrencyParameters low_volatility();
    /// Every other currency (Table 3).
    CurrencyParameters high_volatility();

    /// The sensitivities of a netting set to one currency's curve, by vertex.
    struct Sensitivities
    {
        /// PV01 (§22): the change of value for a rise of one basis point of
        /// the yield at the vertex.
        std::array<Real, vertices> delta{};
        /// Σ σ ∂V/∂σ over the options expiring at the vertex (§10c): vega
        /// times the implied at-the-money volatility it is taken to.
        std::array<Real, vertices> vega{};
        /// Σ SF(expiry) σ ∂V/∂σ (§11a).
        std::array<Real, vertices> curvature{};

        /**
         * @brief Adds the sensitivity of a cash flow to its own zero rate:
         *        spread on the two vertices around its time by linear
         *        interpolation, all on the first or the last vertex beyond
         *        them.
         * @param pv01 change of value for +1 bp of the zero rate at `years`.
         */
        void add_delta(Time years, Real pv01);

        /// Adds an option's vega exposure σ ∂V/∂σ at its expiry, with the
        /// curvature that goes with it, interpolated like a delta.
        void add_vega(Time expiry_years, Real vega_times_vol);
    };

    struct Margin
    {
        Real delta = 0.0;
        Real vega = 0.0;
        Real curvature = 0.0;
        Real total() const { return delta + vega + curvature; }
    };

    /// SF(t) = 0.5 min(1, 14 days / t), the link between vega and gamma of
    /// a vanilla option (§11a); t in years of 365 days.
    Real scaling_function(Time expiry_years);

    /// @throws InvalidInput on a non-finite sensitivity.
    Margin interest_rate_margin(const Sensitivities &s,
                                const CurrencyParameters &currency = regular_well_traded());

} // namespace quantModeling::simm

#endif // QM_RISK_SIMM_HPP
