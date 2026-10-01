#ifndef QM_RISK_REGULATORY_BA_CVA_HPP
#define QM_RISK_REGULATORY_BA_CVA_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling::ba_cva
{

    /**
     * @file ba_cva.hpp
     * @brief BA-CVA, the basic approach to the CVA capital charge (Basel
     *        framework MAR50.13-26, in force since January 2023) —
     *        blueprint/wp/23-xva.md §11.4, lot X0.
     *
     * The charge covers the risk that the CVA itself moves when counterparty
     * spreads move — two thirds of the counterparty losses of 2007-2009,
     * according to the Basel Committee. Per counterparty, a stand-alone
     * charge
     *
     *   SCVA_c = (1/alpha) RW_c Σ_NS M_NS EAD_NS DF_NS                (MAR50.15)
     *
     * where M × EAD × DF stands for the area under the discounted EE profile
     * and RW_c for the volatility of the counterparty's spread; then a
     * one-factor aggregation across counterparties
     *
     *   K_reduced = sqrt((rho Σ_c SCVA_c)² + (1 - rho²) Σ_c SCVA_c²)  (MAR50.14)
     *
     * and the capital is DS × K, DS = 0.65.
     */

    /// MAR50.15: converts the EAD back to an EEPE.
    inline constexpr Real alpha = 1.4;
    /// MAR50.14: correlation of a counterparty's spread with the systematic factor.
    inline constexpr Real rho = 0.5;
    /// MAR50.14: discount scalar DS_BA-CVA.
    inline constexpr Real discount_scalar = 0.65;
    /// MAR50.20: floor on the benefit of hedging in the full version.
    inline constexpr Real beta = 0.25;
    /// MAR50.24: diversification of an index hedge.
    inline constexpr Real index_diversification = 0.7;

    /// The rows of Table 1 (MAR50.16).
    enum class Sector
    {
        Sovereign,
        LocalGovernment,
        Financial,
        BasicMaterialsEnergyIndustrials,
        ConsumerTransportAdministrative,
        TechnologyTelecommunications,
        HealthCareUtilitiesProfessional,
        Other
    };

    enum class CreditQuality
    {
        InvestmentGrade,
        /// High yield and not rated share one column.
        HighYieldOrNotRated
    };

    /// RW_c, Table 1 of MAR50.16.
    Real risk_weight(Sector sector, CreditQuality quality);

    /// DF = (1 - exp(-0.05 M)) / (0.05 M), the supervisory discount factor
    /// averaged over the life of the netting set (MAR50.15, footnote 3).
    /// One in the limit M -> 0.
    Real supervisory_discount_factor(Time maturity);

    struct NettingSetExposure
    {
        /// EAD as computed for counterparty credit risk (SA-CCR or IMM).
        Real ead = 0.0;
        /// Effective maturity M_NS, without the five-year cap.
        Time effective_maturity = 0.0;
        /// An IMM EAD is already discounted: DF_NS = 1.
        bool imm = false;
    };

    /// How a single-name hedge relates to the counterparty (Table 2 of
    /// MAR50.26): r_hc = 100 %, 80 %, 50 %.
    enum class HedgeRelation
    {
        Direct,
        LegallyRelated,
        SameSectorAndRegion
    };

    Real hedge_correlation(HedgeRelation relation);

    /// A single-name CDS bought to hedge the CVA of one counterparty.
    struct SingleNameHedge
    {
        Real notional = 0.0;
        Time remaining_maturity = 0.0;
        /// Sector and quality of the *reference name* of the hedge.
        Sector sector = Sector::Other;
        CreditQuality quality = CreditQuality::HighYieldOrNotRated;
        HedgeRelation relation = HedgeRelation::Direct;
    };

    /// An index CDS bought to hedge CVA across counterparties.
    struct IndexHedge
    {
        Real notional = 0.0;
        Time remaining_maturity = 0.0;
        /// Table 1 risk weight of the constituents (name-weighted average for
        /// a mixed index), *before* the 0.7 of MAR50.24.
        Real constituent_risk_weight = 0.0;
    };

    struct Counterparty
    {
        Sector sector = Sector::Other;
        CreditQuality quality = CreditQuality::HighYieldOrNotRated;
        std::vector<NettingSetExposure> netting_sets;
        std::vector<SingleNameHedge> hedges;
    };

    /// SCVA_c (MAR50.15).
    Real stand_alone_cva(const Counterparty &counterparty);

    /// K_reduced (MAR50.14), before the discount scalar.
    Real k_reduced(const std::vector<Counterparty> &counterparties);

    /**
     * @brief K_hedged (MAR50.21):
     *
     *   sqrt((rho Σ_c (SCVA_c - SNH_c) - IH)² + (1 - rho²) Σ_c (SCVA_c - SNH_c)²
     *        + Σ_c HMA_c)
     *
     * SNH_c = Σ_h r_hc RW_h M_h B_h DF_h      single-name hedges (MAR50.23)
     * IH    = Σ_i RW_i M_i B_i DF_i           index hedges       (MAR50.24)
     * HMA_c = Σ_h (1 - r_hc²)(RW_h M_h B_h DF_h)²  misalignment  (MAR50.25)
     */
    Real k_hedged(const std::vector<Counterparty> &counterparties,
                  const std::vector<IndexHedge> &index_hedges);

    /// Capital under the reduced version: DS × K_reduced. Hedges are ignored.
    Real capital_reduced(const std::vector<Counterparty> &counterparties);

    /// Capital under the full version (MAR50.20):
    /// DS × (beta K_reduced + (1 - beta) K_hedged).
    Real capital_full(const std::vector<Counterparty> &counterparties,
                      const std::vector<IndexHedge> &index_hedges);

} // namespace quantModeling::ba_cva

#endif // QM_RISK_REGULATORY_BA_CVA_HPP
