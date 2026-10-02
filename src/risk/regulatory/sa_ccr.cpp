#include "quantModeling/risk/regulatory/sa_ccr.hpp"

#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace quantModeling::sa_ccr
{
    namespace
    {
        /// Rate of the supervisory duration (CRE52.34).
        constexpr Real duration_rate = 0.05;
        constexpr Time ten_business_days = 10.0 / business_days_per_year;

        /// CRE52.73.
        Real factor_scaling(HedgingSetKind kind)
        {
            switch (kind)
            {
                case HedgingSetKind::Basis:
                    return 0.5;
                case HedgingSetKind::Volatility:
                    return 5.0;
                case HedgingSetKind::Regular:
                    break;
            }
            return 1.0;
        }

        /// A hedging set: its kind, then its name.
        using HedgingSetKey = std::pair<HedgingSetKind, std::string>;

        struct ReferenceAddOn
        {
            SubClass subclass;
            Real effective_notional = 0.0;
        };
        /// Per hedging set, the effective notional of each entity (credit,
        /// equity) or commodity type.
        using ReferenceSets = std::map<HedgingSetKey, std::map<std::string, ReferenceAddOn>>;

        void validate(const Trade &trade)
        {
            if (!(trade.notional > 0.0) || !std::isfinite(trade.notional))
                throw InvalidInput("SA-CCR: trade notional must be finite and > 0");
            if (!(trade.maturity >= 0.0))
                throw InvalidInput("SA-CCR: trade maturity must be >= 0");
            const AssetClass cls = asset_class(trade.subclass);
            if (cls == AssetClass::InterestRate || cls == AssetClass::Credit)
            {
                if (!(trade.start >= 0.0) || !(trade.end >= trade.start))
                    throw InvalidInput("SA-CCR: start and end dates must satisfy 0 <= S <= E");
            }
            if ((cls == AssetClass::InterestRate || cls == AssetClass::ForeignExchange ||
                 cls == AssetClass::Commodity) &&
                trade.hedging_set.empty())
                throw InvalidInput(
                    "SA-CCR: interest rate, FX and commodity trades need a hedging set");
            if ((cls == AssetClass::Credit || cls == AssetClass::Equity ||
                 cls == AssetClass::Commodity) &&
                trade.reference.empty())
                throw InvalidInput("SA-CCR: credit, equity and commodity trades need a reference");
        }

        /// The one-factor aggregation of CRE52.61, CRE52.66 and CRE52.70:
        /// sqrt((Σ rho_k A_k)² + Σ (1 - rho_k²) A_k²).
        Real single_factor_add_on(const ReferenceSets &sets)
        {
            Real total = 0.0;
            for (const auto &[key, references] : sets)
            {
                const Real scaling = factor_scaling(key.first);
                Real systematic = 0.0;
                Real idiosyncratic = 0.0;
                for (const auto &[name, reference] : references)
                {
                    const SupervisoryParameters p = supervisory_parameters(reference.subclass);
                    const Real add_on = scaling * p.factor * reference.effective_notional;
                    systematic += p.correlation * add_on;
                    idiosyncratic += (1.0 - p.correlation * p.correlation) * add_on * add_on;
                }
                total += std::sqrt(systematic * systematic + idiosyncratic);
            }
            return total;
        }

        Exposure assemble(const NettingSet &netting_set, Real rc,
                          const std::optional<MarginAgreement> &margin, Real value_minus_collateral)
        {
            Exposure e;
            e.replacement_cost = rc;
            e.add_ons = add_ons(netting_set.trades, margin);
            e.multiplier = multiplier(value_minus_collateral, e.add_ons.aggregate());
            e.pfe = e.multiplier * e.add_ons.aggregate();
            e.ead = alpha * (e.replacement_cost + e.pfe);
            return e;
        }

        Real value_minus_collateral(const NettingSet &netting_set)
        {
            Real value = 0.0;
            for (const Trade &trade : netting_set.trades)
                value += trade.market_value;
            return value - netting_set.collateral;
        }
    } // namespace

    SupervisoryParameters supervisory_parameters(SubClass subclass)
    {
        // Table 2 of CRE52.72: factor, correlation, option volatility.
        switch (subclass)
        {
            case SubClass::InterestRate:
                return {0.005, 0.0, 0.50};
            case SubClass::ForeignExchange:
                return {0.04, 0.0, 0.15};
            case SubClass::CreditAAA:
            case SubClass::CreditAA:
                return {0.0038, 0.50, 1.00};
            case SubClass::CreditA:
                return {0.0042, 0.50, 1.00};
            case SubClass::CreditBBB:
                return {0.0054, 0.50, 1.00};
            case SubClass::CreditBB:
                return {0.0106, 0.50, 1.00};
            case SubClass::CreditB:
                return {0.016, 0.50, 1.00};
            case SubClass::CreditCCC:
                return {0.06, 0.50, 1.00};
            case SubClass::CreditIndexIG:
                return {0.0038, 0.80, 0.80};
            case SubClass::CreditIndexSG:
                return {0.0106, 0.80, 0.80};
            case SubClass::EquitySingleName:
                return {0.32, 0.50, 1.20};
            case SubClass::EquityIndex:
                return {0.20, 0.80, 0.75};
            case SubClass::CommodityElectricity:
                return {0.40, 0.40, 1.50};
            case SubClass::CommodityOilGas:
            case SubClass::CommodityMetals:
            case SubClass::CommodityAgricultural:
            case SubClass::CommodityOther:
                return {0.18, 0.40, 0.70};
        }
        throw InvalidInput("SA-CCR: unknown sub-class");
    }

    AssetClass asset_class(SubClass subclass)
    {
        switch (subclass)
        {
            case SubClass::InterestRate:
                return AssetClass::InterestRate;
            case SubClass::ForeignExchange:
                return AssetClass::ForeignExchange;
            case SubClass::CreditAAA:
            case SubClass::CreditAA:
            case SubClass::CreditA:
            case SubClass::CreditBBB:
            case SubClass::CreditBB:
            case SubClass::CreditB:
            case SubClass::CreditCCC:
            case SubClass::CreditIndexIG:
            case SubClass::CreditIndexSG:
                return AssetClass::Credit;
            case SubClass::EquitySingleName:
            case SubClass::EquityIndex:
                return AssetClass::Equity;
            case SubClass::CommodityElectricity:
            case SubClass::CommodityOilGas:
            case SubClass::CommodityMetals:
            case SubClass::CommodityAgricultural:
            case SubClass::CommodityOther:
                return AssetClass::Commodity;
        }
        throw InvalidInput("SA-CCR: unknown sub-class");
    }

    Real supervisory_duration(Time start, Time end)
    {
        if (!(start >= 0.0) || !(end >= start))
            throw InvalidInput("SA-CCR: supervisory duration requires 0 <= S <= E");
        const Real duration =
            (std::exp(-duration_rate * start) - std::exp(-duration_rate * end)) / duration_rate;
        return std::max(duration, ten_business_days);
    }

    Real adjusted_notional(const Trade &trade)
    {
        const AssetClass cls = asset_class(trade.subclass);
        if (cls == AssetClass::InterestRate || cls == AssetClass::Credit)
            return trade.notional * supervisory_duration(trade.start, trade.end);
        return trade.notional;
    }

    Real supervisory_delta(const Trade &trade)
    {
        if (!trade.option)
            return trade.long_primary_risk_factor ? 1.0 : -1.0;

        const OptionTerms &o = *trade.option;
        const Real price = o.underlying_price + o.shift;
        const Real strike = o.strike + o.shift;
        if (!(price > 0.0) || !(strike > 0.0))
            throw InvalidInput(
                "SA-CCR: option price and strike must be > 0 after the shift (CRE52.40)");
        if (!(o.exercise > 0.0))
            throw InvalidInput("SA-CCR: option exercise date must be > 0");
        const Real sigma = supervisory_parameters(trade.subclass).option_volatility;
        const Real d = (std::log(price / strike) + 0.5 * sigma * sigma * o.exercise) /
                       (sigma * std::sqrt(o.exercise));
        const Real sign = o.side == OptionSide::Bought ? 1.0 : -1.0;
        return o.type == OptionType::Call ? sign * norm_cdf(d) : -sign * norm_cdf(-d);
    }

    Real maturity_factor_unmargined(Time maturity)
    {
        if (!(maturity >= 0.0))
            throw InvalidInput("SA-CCR: maturity must be >= 0");
        return std::sqrt(std::min(std::max(maturity, ten_business_days), 1.0));
    }

    Real maturity_factor_margined(Time margin_period_of_risk)
    {
        if (!(margin_period_of_risk > 0.0))
            throw InvalidInput("SA-CCR: margin period of risk must be > 0");
        return 1.5 * std::sqrt(margin_period_of_risk);
    }

    Time margin_period_of_risk_floor(int remargining_period_days, int base_floor_days)
    {
        if (remargining_period_days < 1 || base_floor_days < 1)
            throw InvalidInput("SA-CCR: re-margining period and MPoR floor must be >= 1 day");
        return (base_floor_days + remargining_period_days - 1) / business_days_per_year;
    }

    Real multiplier(Real value_minus_collateral, Real aggregate_add_on)
    {
        if (!(aggregate_add_on >= 0.0))
            throw InvalidInput("SA-CCR: aggregate add-on must be >= 0");
        // Nothing to scale; also the limit of the formula for V - C >= 0.
        if (aggregate_add_on == 0.0)
            return 1.0;
        const Real exponent =
            value_minus_collateral / (2.0 * (1.0 - multiplier_floor) * aggregate_add_on);
        return std::min(1.0, multiplier_floor + (1.0 - multiplier_floor) * std::exp(exponent));
    }

    AddOns add_ons(const std::vector<Trade> &trades, const std::optional<MarginAgreement> &margin)
    {
        // Interest rate: per hedging set (currency), the three maturity
        // buckets of CRE52.57 step 3.
        std::map<HedgingSetKey, std::array<Real, 3>> rate_buckets;
        std::map<HedgingSetKey, Real> fx_sets;
        ReferenceSets credit_sets, equity_sets, commodity_sets;

        const auto accumulate = [](ReferenceSets &sets, const HedgingSetKey &key,
                                   const Trade &trade, Real effective_notional)
        {
            auto [it, inserted] =
                sets[key].try_emplace(trade.reference, ReferenceAddOn{trade.subclass, 0.0});
            if (!inserted && it->second.subclass != trade.subclass)
                throw InvalidInput("SA-CCR: reference '" + trade.reference +
                                   "' appears with two different sub-classes");
            it->second.effective_notional += effective_notional;
        };

        for (const Trade &trade : trades)
        {
            validate(trade);
            const Real mf = margin ? maturity_factor_margined(margin->margin_period_of_risk)
                                   : maturity_factor_unmargined(trade.maturity);
            // D = delta × d × MF (step 1 of every asset class).
            const Real effective_notional =
                supervisory_delta(trade) * adjusted_notional(trade) * mf;

            switch (asset_class(trade.subclass))
            {
                case AssetClass::InterestRate:
                {
                    const std::size_t bucket = trade.end < 1.0 ? 0 : (trade.end <= 5.0 ? 1 : 2);
                    auto [it, inserted] = rate_buckets.try_emplace(
                        HedgingSetKey{trade.kind, trade.hedging_set},
                        std::array<Real, 3>{0.0, 0.0, 0.0});
                    it->second[bucket] += effective_notional;
                    break;
                }
                case AssetClass::ForeignExchange:
                    fx_sets[{trade.kind, trade.hedging_set}] += effective_notional;
                    break;
                case AssetClass::Credit:
                    // Regular trades: one hedging set for the asset class.
                    accumulate(credit_sets,
                               {trade.kind, trade.kind == HedgingSetKind::Regular
                                                ? std::string()
                                                : trade.hedging_set},
                               trade, effective_notional);
                    break;
                case AssetClass::Equity:
                    accumulate(equity_sets,
                               {trade.kind, trade.kind == HedgingSetKind::Regular
                                                ? std::string()
                                                : trade.hedging_set},
                               trade, effective_notional);
                    break;
                case AssetClass::Commodity:
                    accumulate(commodity_sets, {trade.kind, trade.hedging_set}, trade,
                               effective_notional);
                    break;
            }
        }

        AddOns result;

        const Real rate_factor = supervisory_parameters(SubClass::InterestRate).factor;
        for (const auto &[key, d] : rate_buckets)
        {
            // CRE52.57 step 5: partial offset between maturity buckets.
            const Real squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 1.4 * d[0] * d[1] +
                                 1.4 * d[1] * d[2] + 0.6 * d[0] * d[2];
            result.interest_rate +=
                factor_scaling(key.first) * rate_factor * std::sqrt(std::max(squared, 0.0));
        }

        const Real fx_factor = supervisory_parameters(SubClass::ForeignExchange).factor;
        for (const auto &[key, effective_notional] : fx_sets)
            result.foreign_exchange +=
                factor_scaling(key.first) * fx_factor * std::abs(effective_notional);

        result.credit = single_factor_add_on(credit_sets);
        result.equity = single_factor_add_on(equity_sets);
        result.commodity = single_factor_add_on(commodity_sets);
        return result;
    }

    Real replacement_cost(const NettingSet &netting_set)
    {
        const Real exposure = value_minus_collateral(netting_set);
        if (!netting_set.margin)
            return std::max(exposure, 0.0);
        const MarginAgreement &m = *netting_set.margin;
        return std::max({exposure, m.threshold + m.minimum_transfer_amount - m.nica, 0.0});
    }

    Exposure exposure_at_default(const NettingSet &netting_set)
    {
        const Real exposure = value_minus_collateral(netting_set);
        Exposure unmargined =
            assemble(netting_set, std::max(exposure, 0.0), std::nullopt, exposure);
        if (!netting_set.margin)
            return unmargined;

        Exposure margined =
            assemble(netting_set, replacement_cost(netting_set), netting_set.margin, exposure);
        // CRE52.2: the margined EAD is capped at the unmargined one.
        if (margined.ead <= unmargined.ead)
            return margined;
        unmargined.capped_at_unmargined = true;
        return unmargined;
    }

    Real exposure_at_default(Real value_minus_collateral, Real add_on_unmargined,
                             const std::optional<MarginAgreement> &margin, Real add_on_margined)
    {
        if (!(add_on_unmargined >= 0.0) || !(add_on_margined >= 0.0))
            throw InvalidInput("SA-CCR: an add-on must be >= 0");
        const auto ead = [value_minus_collateral](Real rc, Real add_on)
        {
            // Nothing left to add on: the multiplier has nothing to scale.
            const Real pfe = add_on > 0.0 ? multiplier(value_minus_collateral, add_on) * add_on : 0.0;
            return alpha * (rc + pfe);
        };
        const Real unmargined = ead(std::max(value_minus_collateral, 0.0), add_on_unmargined);
        if (!margin)
            return unmargined;
        const Real rc = std::max(
            {value_minus_collateral, margin->threshold + margin->minimum_transfer_amount - margin->nica,
             0.0});
        return std::min(ead(rc, add_on_margined), unmargined);
    }

} // namespace quantModeling::sa_ccr
