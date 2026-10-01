#ifndef QM_RISK_REGULATORY_SA_CCR_HPP
#define QM_RISK_REGULATORY_SA_CCR_HPP

#include "quantModeling/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace quantModeling::sa_ccr
{

    /**
     * @file sa_ccr.hpp
     * @brief SA-CCR, the standardised approach to counterparty credit risk
     *        (Basel framework CRE52; BCBS 279, March 2014) —
     *        blueprint/wp/23-xva.md §11.1, lot X0.
     *
     *   EAD = alpha (RC + PFE),   alpha = 1.4                        (CRE52.1)
     *   PFE = multiplier × AddOn_aggregate                           (CRE52.20)
     *
     * RC is the replacement cost today, PFE a stylised Effective EPE of the
     * netting set: per trade an effective notional D = delta × d × MF
     * (direction and moneyness × size × time horizon), aggregated inside
     * hedging sets with supervisory offsetting rules, then multiplied by a
     * supervisory factor. No diversification is recognised between asset
     * classes.
     *
     * Every number below is the one of the primary text, with its paragraph.
     * The acceptance test reproduces the five worked examples of BCBS 279
     * Annex 4a (CRE99.20 onwards) to the unit.
     *
     * Not covered, because the library has no such product: CDO tranches
     * (CRE52.41), the digital-option collar (CRE52.42), netting sets under
     * several margin agreements (CRE52.74-76).
     */

    /// CRE52.1.
    inline constexpr Real alpha = 1.4;
    /// CRE52.23: the multiplier never goes below 5 % of the add-on.
    inline constexpr Real multiplier_floor = 0.05;
    /// CRE52.49, CRE52.53: the market convention the text itself uses.
    inline constexpr Real business_days_per_year = 250.0;

    enum class AssetClass
    {
        InterestRate,
        ForeignExchange,
        Credit,
        Equity,
        Commodity
    };

    /// The rows of Table 2 (CRE52.72).
    enum class SubClass
    {
        InterestRate,
        ForeignExchange,
        CreditAAA,
        CreditAA,
        CreditA,
        CreditBBB,
        CreditBB,
        CreditB,
        CreditCCC,
        CreditIndexIG,
        CreditIndexSG,
        EquitySingleName,
        EquityIndex,
        CommodityElectricity,
        CommodityOilGas,
        CommodityMetals,
        CommodityAgricultural,
        CommodityOther
    };

    struct SupervisoryParameters
    {
        /// SF: supervisory move of the risk factor over one year.
        Real factor;
        /// rho: weight of the systematic factor (0 where the text says N/A).
        Real correlation;
        /// sigma: volatility to use in the option delta.
        Real option_volatility;
    };

    /// Table 2 of CRE52.72.
    SupervisoryParameters supervisory_parameters(SubClass subclass);
    AssetClass asset_class(SubClass subclass);

    enum class OptionType
    {
        Call,
        Put
    };
    enum class OptionSide
    {
        Bought,
        Sold
    };

    struct OptionTerms
    {
        OptionType type;
        OptionSide side;
        /// P: spot, forward, average... of the underlying.
        Real underlying_price;
        /// K.
        Real strike;
        /// T: latest contractual exercise date, in years.
        Time exercise;
        /// lambda of CRE52.40 FAQ: shift added to P and K when rates may be
        /// negative. Zero otherwise.
        Real shift = 0.0;
    };

    /// CRE52.45-47, CRE52.73: basis and volatility transactions sit in their
    /// own hedging sets, with the supervisory factor × 0.5 and × 5.
    enum class HedgingSetKind
    {
        Regular,
        Basis,
        Volatility
    };

    struct Trade
    {
        SubClass subclass;
        /// Interest rate: the currency. FX: the currency pair. Commodity:
        /// "Energy", "Metals", "Agricultural" or "Other". Credit and equity:
        /// unused for regular trades (one hedging set per asset class).
        std::string hedging_set;
        /// Credit and equity: the entity or the index. Commodity: the
        /// commodity type ("Crude Oil"). Unused for interest rate and FX.
        std::string reference;
        /// Interest rate and credit: the trade notional. FX: the notional of
        /// the foreign leg in domestic currency. Equity and commodity: price
        /// × number of units (CRE52.34-36). Always > 0: the direction is in
        /// the delta.
        Real notional = 0.0;
        /// Ignored for an option, whose delta carries the sign. "Long" means
        /// the value rises with the primary risk factor (CRE52.39): a payer
        /// swap, bought CDS protection, a long forward.
        bool long_primary_risk_factor = true;
        /// M: time until the contract may last be active (CRE52.31).
        Time maturity = 0.0;
        /// S and E: the period the underlying rate or credit refers to
        /// (CRE52.34). Interest rate and credit only. A 1Y-into-10Y swaption
        /// has S = 1, E = 11.
        Time start = 0.0;
        Time end = 0.0;
        /// Current market value, summed into V of the netting set.
        Real market_value = 0.0;
        std::optional<OptionTerms> option;
        HedgingSetKind kind = HedgingSetKind::Regular;
    };

    /// A margin agreement under which the *counterparty* posts variation
    /// margin (CRE52.2: a one-way agreement in the bank's disfavour is
    /// unmargined).
    struct MarginAgreement
    {
        /// TH.
        Real threshold = 0.0;
        /// MTA.
        Real minimum_transfer_amount = 0.0;
        /// NICA: net independent collateral amount, i.e. initial margin and
        /// independent amounts held, net of those posted unsegregated
        /// (CRE52.17).
        Real nica = 0.0;
        /// MPoR in years, after the floors of CRE52.50-51.
        Time margin_period_of_risk = 10.0 / business_days_per_year;
    };

    struct NettingSet
    {
        std::vector<Trade> trades;
        /// C: haircut value of the net collateral held, variation margin and
        /// NICA together. Negative when the bank is a net poster.
        Real collateral = 0.0;
        std::optional<MarginAgreement> margin;
    };

    struct AddOns
    {
        Real interest_rate = 0.0;
        Real foreign_exchange = 0.0;
        Real credit = 0.0;
        Real equity = 0.0;
        Real commodity = 0.0;

        /// CRE52.25: a plain sum, no diversification across asset classes.
        Real aggregate() const
        {
            return interest_rate + foreign_exchange + credit + equity + commodity;
        }
    };

    struct Exposure
    {
        Real replacement_cost = 0.0;
        AddOns add_ons;
        Real multiplier = 1.0;
        Real pfe = 0.0;
        Real ead = 0.0;
        /// True when the margined EAD was above the EAD of the same netting
        /// set treated as unmargined, which then applies (CRE52.2). The other
        /// fields are in that case the unmargined ones.
        bool capped_at_unmargined = false;
    };

    // ── Building blocks, exposed because each one is a testable formula ──────

    /// SD = (exp(-0.05 S) - exp(-0.05 E)) / 0.05, floored at ten business
    /// days (CRE52.34).
    Real supervisory_duration(Time start, Time end);

    /// d (CRE52.34-36): notional × SD for interest rate and credit, the
    /// notional itself otherwise.
    Real adjusted_notional(const Trade &trade);

    /// delta (CRE52.39-40): ±1 for a linear trade; for an option
    ///   call bought +Φ(d),  call sold -Φ(d),
    ///   put bought  -Φ(-d), put sold  +Φ(-d),
    ///   d = (ln((P + λ)/(K + λ)) + σ² T / 2) / (σ sqrt(T)).
    Real supervisory_delta(const Trade &trade);

    /// MF = sqrt(min(M, 1 year) / 1 year), M floored at ten business days
    /// (CRE52.48).
    Real maturity_factor_unmargined(Time maturity);

    /// MF = 3/2 sqrt(MPoR / 1 year) (CRE52.52).
    Real maturity_factor_margined(Time margin_period_of_risk);

    /// The MPoR floor of CRE52.50 in years: ten business days with daily
    /// margining, nine plus the re-margining period otherwise.
    /// `base_floor_days` is 20 in the cases of CRE52.51.
    Time margin_period_of_risk_floor(int remargining_period_days, int base_floor_days = 10);

    /// CRE52.23:
    ///   min(1, Floor + (1 - Floor) exp((V - C) / (2 (1 - Floor) AddOn))).
    /// One when the netting set is under-collateralised, below one when it is
    /// over-collateralised or out of the money.
    Real multiplier(Real value_minus_collateral, Real aggregate_add_on);

    /// The asset-class add-ons of CRE52.56-71. `margined` selects the
    /// maturity factor.
    AddOns add_ons(const std::vector<Trade> &trades,
                   const std::optional<MarginAgreement> &margin = std::nullopt);

    /// RC (CRE52.10, CRE52.18): max(V - C, 0) unmargined,
    /// max(V - C, TH + MTA - NICA, 0) margined.
    Real replacement_cost(const NettingSet &netting_set);

    /// The whole calculation.
    /// @throws InvalidInput on a non-positive notional, inconsistent dates,
    ///         an option without a positive (shifted) price and strike, or
    ///         one reference given two different sub-classes.
    Exposure exposure_at_default(const NettingSet &netting_set);

} // namespace quantModeling::sa_ccr

#endif // QM_RISK_REGULATORY_SA_CCR_HPP
