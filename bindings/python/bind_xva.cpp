// Python bindings, xva (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/market/credit_bootstrap.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/xva_market_risks.hpp"
#include "quantModeling/market/historical_rate_dynamics.hpp"
#include "quantModeling/engines/xva/script_future_value.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/risk/xva_report.hpp"
#include "quantModeling/utils/thread_pool.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

// ── xVA: exposure, collateral, CVA / DVA of a netting set (blueprint/wp/23-xva.md) ──
//
// One call runs the whole chain on common paths: Hull-White exposure cube,
// collateral, exposure statistics, adjustments and each trade's share; and,
// when historical dynamics are given, a second cube for the risk measures.

namespace
{
    template <class T>
    T item_or(const py::dict &d, const char *key, T fallback)
    {
        return d.contains(key) && !d[key].is_none() ? d[key].cast<T>() : fallback;
    }

    qm_::CreditCurve credit_curve(const std::pair<std::vector<qm_::Time>, std::vector<qm_::Real>> &pillars)
    {
        return qm_::CreditCurve(pillars.first, pillars.second);
    }

    /// The terms of a CSA from its dict; an absent key keeps Csa's default.
    qm_::Csa csa_terms(const py::dict &c)
    {
        qm_::Csa terms;
        terms.threshold_counterparty = item_or<qm_::Real>(c, "threshold_counterparty", terms.threshold_counterparty);
        terms.threshold_bank = item_or<qm_::Real>(c, "threshold_bank", terms.threshold_bank);
        terms.minimum_transfer_amount =
            item_or<qm_::Real>(c, "minimum_transfer_amount", terms.minimum_transfer_amount);
        terms.rounding = item_or<qm_::Real>(c, "rounding", terms.rounding);
        terms.independent_amount = item_or<qm_::Real>(c, "independent_amount", terms.independent_amount);
        terms.margin_period_of_risk = item_or<qm_::Time>(c, "margin_period_of_risk", terms.margin_period_of_risk);
        return terms;
    }

    /// The treatment of the trade flows during the margin period of risk, from
    /// the CSA dict's "cashflows" (paid, withheld, only_bank_pays).
    qm_::MarginPeriodCashflows margin_period_cashflows(const py::dict &c)
    {
        const auto flows = item_or<std::string>(c, "cashflows", "paid");
        if (flows == "withheld")
            return qm_::MarginPeriodCashflows::Withheld;
        if (flows == "only_bank_pays")
            return qm_::MarginPeriodCashflows::OnlyBankPays;
        if (flows != "paid")
            throw qm_::InvalidInput("xva: unknown cash-flow treatment '" + flows + "'");
        return qm_::MarginPeriodCashflows::Paid;
    }

    py::dict exposure_dict(const qm_::ExposureStatistics &s)
    {
        py::dict out;
        out["times"] = s.times;
        out["ee"] = s.ee;
        out["ene"] = s.ene;
        out["efv"] = s.efv;
        out["pfe"] = s.pfe;
        out["pfe_confidence"] = s.pfe_confidence;
        out["quantile_levels"] = s.quantile_levels;
        out["value_quantiles"] = s.value_quantiles;
        out["discounted_ee"] = s.discounted_ee;
        out["discounted_ene"] = s.discounted_ene;
        out["discounted_ee_error"] = s.discounted_ee_error;
        out["discounted_ene_error"] = s.discounted_ene_error;
        out["epe"] = s.epe;
        out["eepe"] = s.eepe;
        out["epe_error"] = s.epe_error;
        out["eepe_error"] = s.eepe_error;
        out["pfe_error"] = s.pfe_error;
        out["value_today"] = s.value_today;
        return out;
    }

    py::dict estimate_dict(const qm_::Estimate &e)
    {
        py::dict out;
        out["value"] = e.value;
        out["error"] = e.error;
        return out;
    }

    py::dict report_dict(const qm_::XvaReport &r)
    {
        py::dict out;
        out["exposure"] = exposure_dict(r.exposure);
        out["cva"] = estimate_dict(r.cva);
        out["dva"] = estimate_dict(r.dva);
        // Equal to cva / dva unless a wrong-way hazard was asked for.
        out["cva_independent"] = estimate_dict(r.cva_independent);
        out["dva_independent"] = estimate_dict(r.dva_independent);
        out["cva_unilateral"] = r.cva_unilateral;
        out["fca"] = r.fca;
        out["fba"] = r.fba;
        out["cva_rule_of_thumb"] = r.cva_rule_of_thumb;
        out["colva"] = r.colva;
        out["mva"] = r.mva;
        out["kva"] = r.kva;
        // Monte-Carlo standard errors (risk/xva_report.hpp).
        out["fca_error"] = r.fca_error;
        out["fba_error"] = r.fba_error;
        out["fva_error"] = r.fva_error;
        out["colva_error"] = r.colva_error;
        out["mva_error"] = r.mva_error;
        out["kva_error"] = r.kva_error;
        if (r.expected_initial_margin.empty())
            out["initial_margin"] = py::none();
        else
        {
            py::dict margin;
            margin["today"] = r.initial_margin_today;
            margin["expected"] = r.expected_initial_margin;
            out["initial_margin"] = margin;
        }
        if (!r.capital)
            out["capital"] = py::none();
        else
        {
            const qm_::CapitalProfile &k = *r.capital;
            py::dict capital;
            capital["times"] = k.times;
            capital["ead_today"] = k.ead_today;
            capital["default_capital_today"] = k.default_capital_today;
            capital["cva_capital_today"] = k.cva_capital_today;
            capital["expected_ead"] = k.expected_ead;
            capital["discounted_capital"] = k.discounted_capital;
            capital["discounted_default_capital"] = k.discounted_default_capital;
            capital["discounted_cva_capital"] = k.discounted_cva_capital;
            out["capital"] = capital;
        }
        py::list contributions;
        for (const qm_::TradeContribution &c : r.contributions)
        {
            py::dict row;
            row["trade"] = c.trade;
            row["standalone_cva"] = c.standalone_cva;
            row["incremental_cva"] = c.incremental_cva;
            // NaN under a CSA: no exact allocation exists.
            row["marginal_cva"] = std::isnan(c.marginal_cva) ? py::object(py::none()) : py::cast(c.marginal_cva);
            contributions.append(row);
        }
        out["contributions"] = contributions;
        return out;
    }
} // namespace

namespace
{
    qm_::ba_cva::Sector ba_cva_sector(const std::string &name)
    {
        using S = qm_::ba_cva::Sector;
        static const std::map<std::string, S> sectors = {
            {"sovereign", S::Sovereign},
            {"local_government", S::LocalGovernment},
            {"financial", S::Financial},
            {"basic_materials_energy_industrials", S::BasicMaterialsEnergyIndustrials},
            {"consumer_transport_administrative", S::ConsumerTransportAdministrative},
            {"technology_telecommunications", S::TechnologyTelecommunications},
            {"health_care_utilities_professional", S::HealthCareUtilitiesProfessional},
            {"other", S::Other},
        };
        const auto it = sectors.find(name);
        if (it == sectors.end())
            throw qm_::InvalidInput("xva: unknown BA-CVA sector '" + name + "'");
        return it->second;
    }

    /// One trade of the netting set as SA-CCR describes it (CRE52.32, Table 1):
    /// a swap by its start and end; a swaption by its underlying swap, with
    /// the option's terms; a Bermudan with S its first exercise date and T
    /// its last (CRE52.42).
    qm_::sa_ccr::Trade sa_ccr_trade(const std::string &kind, const qm_::InterestRateSwap &swap,
                                    qm_::Time expiry, qm_::Time last_exercise, qm_::Real quantity,
                                    const qm_::DiscountCurve &curve)
    {
        qm_::sa_ccr::Trade t;
        t.subclass = qm_::sa_ccr::SubClass::InterestRate;
        t.hedging_set = "USD";
        t.notional = swap.notional * std::abs(quantity);
        t.maturity = swap.maturity();
        t.start = swap.start();
        t.end = swap.maturity();
        // A payer swap gains when rates rise; a negative quantity is the
        // other side.
        const bool long_rates = swap.payer == (quantity > 0.0);
        if (kind == "swap")
        {
            t.long_primary_risk_factor = long_rates;
            return t;
        }
        qm_::sa_ccr::OptionTerms option;
        // A payer swaption is a call on the swap rate, a receiver a put.
        option.type = swap.payer ? qm_::sa_ccr::OptionType::Call : qm_::sa_ccr::OptionType::Put;
        option.side = quantity > 0.0 ? qm_::sa_ccr::OptionSide::Bought : qm_::sa_ccr::OptionSide::Sold;
        option.underlying_price = qm_::value_swap(swap, qm_::MultiCurve{curve, curve}).par_rate;
        option.strike = swap.fixed_rate;
        option.exercise = kind == "bermudan" ? last_exercise : expiry;
        t.option = option;
        return t;
    }
} // namespace

namespace
{
    /// The supervisory description of an amount already fixed: the coupon in
    /// progress of a swap of `notional` that ends at `swap_end`. The swap is
    /// one interest rate trade from today to its end (CRE52.34), and the
    /// engine holds it as two: this coupon, and the swap that starts on the
    /// coupon's date. The two descriptions must add up to the swap's — its
    /// supervisory duration, in its own maturity bucket. The part after the
    /// coupon has the duration of (time, end); this one is given the end of
    /// the swap too, and the start S that leaves it the duration of
    /// (0, time): e^{-0.05 S} = e^{-0.05 end} + 1 - e^{-0.05 time}.
    qm_::sa_ccr::Trade cash_flow_regulatory(const py::dict &t, qm_::Time time, qm_::Real quantity,
                                            const std::string &hedging_set, qm_::Real to_domestic)
    {
        qm_::sa_ccr::Trade r;
        r.subclass = qm_::sa_ccr::SubClass::InterestRate;
        r.hedging_set = hedging_set;
        r.notional = item_or<qm_::Real>(t, "notional", std::abs(t["amount"].cast<qm_::Real>())) *
                     std::abs(quantity) * to_domestic;
        const qm_::Time end = std::max(item_or<qm_::Time>(t, "swap_end", time), time);
        r.end = end;
        r.maturity = end;
        r.start = -std::log(std::exp(-0.05 * end) + 1.0 - std::exp(-0.05 * time)) / 0.05;
        r.long_primary_risk_factor = item_or<bool>(t, "payer", true) == (quantity > 0.0);
        return r;
    }

    /// Adds the trades of a request to the engine (parsed while the GIL is
    /// held; the engine copies them), with the SA-CCR description of each.
    /// `scripts_have_capital`: a script has no such description, and is only
    /// accepted when no capital is asked for or the method needs none.
    void add_xva_trades(qm_::HullWhiteExposureEngine &engine, const py::list &trades,
                        const qm_::HullWhiteCurveModel &model, const qm_::DiscountCurve &curve,
                        bool scripts_have_capital, std::vector<qm_::sa_ccr::Trade> &regulatory_trades)
    {
        for (const py::handle &h : trades)
        {
            const py::dict t = h.cast<py::dict>();
            const auto kind = t["kind"].cast<std::string>();
            const qm_::Real quantity = item_or<qm_::Real>(t, "quantity", 1.0);
            const qm_::Time expiry = item_or<qm_::Time>(t, "expiry", 0.0);
            if (kind == "cashflow")
            {
                // An amount already fixed: the coupon in progress of a swap
                // that has started.
                const qm_::Time time = t["time"].cast<qm_::Time>();
                engine.add(qm_::make_cash_flow_future_value(time, t["amount"].cast<qm_::Real>(), model),
                           quantity);
                regulatory_trades.push_back(cash_flow_regulatory(t, time, quantity, "USD", 1.0));
                continue;
            }
            if (kind == "script")
            {
                // A trade written in the payoff language, on rates only: its
                // exposure by regression of what it pays (lot X4b). SA-CCR has
                // no description of an arbitrary script.
                if (!scripts_have_capital)
                    throw qm_::InvalidInput(
                        "xva: the standardised approach has no SA-CCR description of a scripted trade; "
                        "ask for the capital by the internal models method (capital method "
                        "'internal_model'), which reads the exposure off the simulation");
                const qm_::ValuationContext ctx{
                    qm_::Date::from_iso(t["valuation_date"].cast<std::string>())};
                engine.add(qm_::make_script_future_value(t["script"].cast<std::string>(), ctx, model),
                           quantity);
                continue;
            }
            const bool is_swap = kind == "swap";
            if (!is_swap && kind != "swaption" && kind != "bermudan")
                throw qm_::InvalidInput("xva: unknown trade kind '" + kind +
                                        "' (swap, swaption, bermudan, script)");
            const qm_::Time tenor = t["tenor"].cast<qm_::Time>();
            const qm_::InterestRateSwap swap = qm_::make_swap(
                is_swap ? item_or<qm_::Time>(t, "start", 0.0) : expiry, tenor, t["fixed_rate"].cast<qm_::Real>(),
                item_or<int>(t, "fixed_frequency", 1), item_or<int>(t, "float_frequency", 1),
                item_or<qm_::Real>(t, "notional", 1.0), item_or<bool>(t, "payer", true));
            qm_::Time last_exercise = expiry;
            if (is_swap)
                engine.add(swap, quantity);
            else if (kind == "swaption")
                engine.add(qm_::Swaption(swap, expiry), quantity);
            else
            {
                // Exercisable on `expiry` and on each anniversary while a full
                // year of the swap is left.
                std::vector<qm_::Time> exercise;
                for (qm_::Time e = expiry; e < expiry + tenor - 1e-9; e += 1.0)
                    exercise.push_back(e);
                last_exercise = exercise.back();
                engine.add(qm_::BermudanSwaption(swap, exercise), quantity);
            }
            regulatory_trades.push_back(sa_ccr_trade(kind, swap, expiry, last_exercise, quantity, curve));
        }
    }

    /// The trades of a two-currency netting set (lot X10): rate trades of
    /// either currency, FX forwards and cross-currency swaps, each with its
    /// SA-CCR description. An FX trade's adjusted notional is that of its
    /// foreign leg in domestic currency (CRE52.34), its hedging set the pair.
    ///
    /// When the "foreign currency" is an asset (lot X14, `asset` true), the
    /// only trade on it is a European option, kind 'asset_option': an equity
    /// trade for SA-CCR, by the price of the units it is on (CRE52.36).
    void add_two_currency_trades(qm_::CrossCurrencyExposureEngine &engine, const py::list &trades,
                                 const qm_::CrossCurrencyHullWhiteModel &model, const std::string &pair,
                                 const std::string &foreign_currency,
                                 std::vector<qm_::sa_ccr::Trade> &regulatory_trades, bool asset = false,
                                 bool asset_is_index = false)
    {
        const auto fx_trade = [&](qm_::Real foreign_notional, bool long_foreign, qm_::Time maturity)
        {
            qm_::sa_ccr::Trade t;
            t.subclass = qm_::sa_ccr::SubClass::ForeignExchange;
            t.hedging_set = pair;
            t.notional = foreign_notional * model.spot();
            t.long_primary_risk_factor = long_foreign;
            t.maturity = maturity;
            return t;
        };
        for (const py::handle &h : trades)
        {
            const py::dict t = h.cast<py::dict>();
            const auto kind = t["kind"].cast<std::string>();
            const qm_::Real quantity = item_or<qm_::Real>(t, "quantity", 1.0);
            if (kind == "asset_option")
            {
                if (!asset)
                    throw qm_::InvalidInput("xva: an asset_option needs the asset of the netting set "
                                            "(foreign.dividend_yield)");
                const qm_::Time expiry = t["expiry"].cast<qm_::Time>();
                const qm_::Real strike = t["strike"].cast<qm_::Real>();
                const qm_::Real units = item_or<qm_::Real>(t, "notional", 1.0);
                const bool call = item_or<bool>(t, "is_call", true);
                engine.add(qm_::VanillaOption(
                               std::make_shared<qm_::PlainVanillaPayoff>(
                                   call ? qm_::OptionType::Call : qm_::OptionType::Put, strike),
                               std::make_shared<qm_::EuropeanExercise>(expiry), units),
                           quantity);
                qm_::sa_ccr::Trade r;
                r.subclass = asset_is_index ? qm_::sa_ccr::SubClass::EquityIndex
                                            : qm_::sa_ccr::SubClass::EquitySingleName;
                r.reference = foreign_currency;
                r.notional = units * std::abs(quantity) * model.spot();
                r.maturity = expiry;
                qm_::sa_ccr::OptionTerms option;
                option.type = call ? qm_::sa_ccr::OptionType::Call : qm_::sa_ccr::OptionType::Put;
                option.side = quantity > 0.0 ? qm_::sa_ccr::OptionSide::Bought : qm_::sa_ccr::OptionSide::Sold;
                option.underlying_price = model.spot();
                option.strike = strike;
                option.exercise = expiry;
                r.option = option;
                regulatory_trades.push_back(r);
                continue;
            }
            if (kind == "cashflow")
            {
                const bool in_foreign = item_or<std::string>(t, "currency", "domestic") == "foreign";
                const qm_::Time time = t["time"].cast<qm_::Time>();
                const qm_::Real amount = t["amount"].cast<qm_::Real>();
                if (in_foreign)
                    engine.add(qm_::in_foreign_currency(
                                   qm_::make_cash_flow_future_value(time, amount, model.foreign()), model.spot()),
                               quantity);
                else
                    engine.add(qm_::in_domestic_currency(
                                   qm_::make_cash_flow_future_value(time, amount, model.domestic())),
                               quantity);
                regulatory_trades.push_back(cash_flow_regulatory(
                    t, time, quantity, in_foreign ? foreign_currency : std::string("USD"),
                    in_foreign ? model.spot() : 1.0));
                continue;
            }
            if (asset && (kind == "fx_forward" || kind == "cross_currency_swap" ||
                          item_or<std::string>(t, "currency", "domestic") == "foreign"))
                throw qm_::InvalidInput("xva: with an asset in the netting set the other trades are "
                                        "rate trades in the domestic currency");
            if (kind == "fx_forward")
            {
                const qm_::Time maturity = t["maturity"].cast<qm_::Time>();
                const qm_::Real notional = item_or<qm_::Real>(t, "notional", 1.0);
                engine.add(qm_::FXForward(t["strike"].cast<qm_::Real>(), maturity, notional), quantity);
                regulatory_trades.push_back(fx_trade(notional * std::abs(quantity), quantity > 0.0, maturity));
                continue;
            }
            if (kind == "cross_currency_swap")
            {
                const qm_::Time maturity = t["maturity"].cast<qm_::Time>();
                const qm_::Real foreign_notional = t["foreign_notional"].cast<qm_::Real>();
                const bool receive_foreign = item_or<bool>(t, "receive_foreign", true);
                engine.add(qm_::CrossCurrencySwap(maturity, foreign_notional,
                                                  t["domestic_notional"].cast<qm_::Real>(),
                                                  t["foreign_rate"].cast<qm_::Real>(),
                                                  t["domestic_rate"].cast<qm_::Real>(),
                                                  item_or<int>(t, "frequency", 1), receive_foreign),
                           quantity);
                regulatory_trades.push_back(fx_trade(foreign_notional * std::abs(quantity),
                                                     receive_foreign == (quantity > 0.0), maturity));
                continue;
            }
            if (kind != "swap" && kind != "swaption")
                throw qm_::InvalidInput("xva: with two currencies a trade is a swap, a swaption, an "
                                        "fx_forward or a cross_currency_swap, not '" +
                                        kind + "'");
            const auto currency = item_or<std::string>(t, "currency", "domestic");
            if (currency != "domestic" && currency != "foreign")
                throw qm_::InvalidInput("xva: a trade's currency is 'domestic' or 'foreign'");
            const bool foreign = currency == "foreign";
            const qm_::Time expiry = item_or<qm_::Time>(t, "expiry", 0.0);
            const bool is_swap = kind == "swap";
            const qm_::InterestRateSwap swap = qm_::make_swap(
                is_swap ? item_or<qm_::Time>(t, "start", 0.0) : expiry, t["tenor"].cast<qm_::Time>(),
                t["fixed_rate"].cast<qm_::Real>(), item_or<int>(t, "fixed_frequency", 1),
                item_or<int>(t, "float_frequency", 1), item_or<qm_::Real>(t, "notional", 1.0),
                item_or<bool>(t, "payer", true));
            if (is_swap)
                foreign ? engine.add_foreign(swap, quantity) : engine.add_domestic(swap, quantity);
            else
                foreign ? engine.add_foreign(qm_::Swaption(swap, expiry), quantity)
                        : engine.add_domestic(qm_::Swaption(swap, expiry), quantity);
            const qm_::HullWhiteCurveModel &rates = foreign ? model.foreign() : model.domestic();
            qm_::sa_ccr::Trade regulatory =
                sa_ccr_trade(kind, swap, expiry, expiry, quantity, rates.discount());
            if (foreign)
            {
                // Its own hedging set, its notional in domestic currency.
                regulatory.hedging_set = foreign_currency;
                regulatory.notional *= model.spot();
            }
            regulatory_trades.push_back(regulatory);
        }
    }
} // namespace

/// The spot volatility of the two-currency model that reprices an FX option
/// of this expiry, and the Black volatilities the model then gives at the
/// other expiries (models/rates/cross_currency_hull_white.hpp).
static py::dict cross_currency_fx_volatility_impl(std::pair<qm_::Real, qm_::Real> domestic_hull_white,
                                                  std::pair<qm_::Real, qm_::Real> foreign_hull_white,
                                                  std::tuple<qm_::Real, qm_::Real, qm_::Real> correlations,
                                                  qm_::Time expiry, qm_::Real market_volatility,
                                                  const std::vector<qm_::Time> &expiries)
{
    // The curves play no part in a volatility.
    const qm_::HullWhiteCurveModel domestic(domestic_hull_white.first, domestic_hull_white.second,
                                            qm_::DiscountCurve(0.0));
    const qm_::HullWhiteCurveModel foreign(foreign_hull_white.first, foreign_hull_white.second,
                                           qm_::DiscountCurve(0.0));
    const qm_::CrossCurrencyHullWhiteModel::Correlations rho{std::get<0>(correlations),
                                                             std::get<1>(correlations),
                                                             std::get<2>(correlations)};
    const qm_::Real spot_volatility =
        qm_::calibrated_fx_volatility(domestic, foreign, rho, expiry, market_volatility);
    const qm_::CrossCurrencyHullWhiteModel model(domestic, foreign, 1.0, spot_volatility, rho);
    std::vector<qm_::Real> implied;
    for (const qm_::Time t : expiries)
        implied.push_back(model.fx_implied_volatility(t));
    py::dict out;
    out["spot_volatility"] = spot_volatility;
    out["implied_volatilities"] = implied;
    return out;
}

static py::dict xva_netting_set_impl(std::vector<qm_::Time> dt, std::vector<qm_::Real> dd,
                                     std::pair<qm_::Real, qm_::Real> hull_white, const py::list &trades,
                                     const std::pair<std::vector<qm_::Time>, std::vector<qm_::Real>> &counterparty_hazard,
                                     const std::pair<std::vector<qm_::Time>, std::vector<qm_::Real>> &own_hazard,
                                     qm_::Real lgd_counterparty, qm_::Real lgd_own, const py::object &csa,
                                     qm_::Real borrowing_spread, qm_::Real lending_spread,
                                     std::optional<std::tuple<qm_::Real, qm_::Real, qm_::Real>> historical,
                                     std::size_t paths, std::uint64_t seed, qm_::Real pfe_confidence,
                                     std::size_t threads, const py::object &initial_margin,
                                     qm_::Real collateral_spread, const py::object &capital,
                                     qm_::Real wrong_way_b, const std::string &device,
                                     const py::object &foreign)
{
    const qm_::DiscountCurve curve = rate_curve(std::move(dt), std::move(dd));
    const qm_::HullWhiteCurveModel model(hull_white.first, hull_white.second, curve);

    // How the exposure at default is measured (risk/capital.hpp): from a
    // supervisory description of each trade, or off the simulation itself.
    bool internal_model = false;
    if (!capital.is_none())
    {
        const auto method = item_or<std::string>(capital.cast<py::dict>(), "method", "sa_ccr");
        if (method != "sa_ccr" && method != "internal_model")
            throw qm_::InvalidInput("xva: unknown capital method '" + method +
                                    "' (sa_ccr, internal_model)");
        internal_model = method == "internal_model";
    }

    // Instruments are parsed while the GIL is held; the engine copies them.
    qm_::HullWhiteExposureEngine engine(model);
    std::vector<qm_::sa_ccr::Trade> regulatory_trades;
    // A second currency (lot X10): its own Hull-White rate and the exchange
    // rate, and the engine that simulates the three.
    std::optional<qm_::CrossCurrencyHullWhiteModel> two_currencies;
    std::optional<qm_::CrossCurrencyExposureEngine> two_currency_engine;
    if (foreign.is_none())
        add_xva_trades(engine, trades, model, curve, capital.is_none() || internal_model, regulatory_trades);
    else
    {
        if (historical)
            throw qm_::InvalidInput("xva: the historical measure is not available with two currencies");
        const py::dict f = foreign.cast<py::dict>();
        // An asset with a known yield in place of the second currency
        // (lot X14): its price is the "exchange rate".
        const bool asset = f.contains("dividend_yield");
        if (asset)
            two_currencies.emplace(qm_::make_asset_model(
                model, f["spot"].cast<qm_::Real>(), f["dividend_yield"].cast<qm_::Real>(),
                f["volatility"].cast<qm_::Real>(), item_or<qm_::Real>(f, "rate_correlation", 0.0)));
        else
        {
            const auto hw = f["hull_white"].cast<std::pair<qm_::Real, qm_::Real>>();
            const auto rho = f["correlations"].cast<std::tuple<qm_::Real, qm_::Real, qm_::Real>>();
            two_currencies.emplace(
                model,
                qm_::HullWhiteCurveModel(hw.first, hw.second,
                                         rate_curve(f["discount_times"].cast<std::vector<qm_::Time>>(),
                                                    f["discount_factors"].cast<std::vector<qm_::Real>>())),
                f["spot"].cast<qm_::Real>(), f["fx_volatility"].cast<qm_::Real>(),
                qm_::CrossCurrencyHullWhiteModel::Correlations{std::get<0>(rho), std::get<1>(rho),
                                                               std::get<2>(rho)});
        }
        two_currency_engine.emplace(*two_currencies);
        const auto currency = item_or<std::string>(f, "currency", asset ? "asset" : "EUR");
        add_two_currency_trades(*two_currency_engine, trades, *two_currencies,
                                currency + "/" + item_or<std::string>(f, "domestic_currency", "USD"),
                                currency, regulatory_trades, asset, item_or<bool>(f, "index", false));
    }

    qm_::XvaInputs inputs;
    inputs.counterparty = credit_curve(counterparty_hazard);
    inputs.own = credit_curve(own_hazard);
    inputs.lgd_counterparty = lgd_counterparty;
    inputs.lgd_own = lgd_own;
    inputs.borrowing_spread = borrowing_spread;
    inputs.lending_spread = lending_spread;
    inputs.pfe_confidence = pfe_confidence;
    // The distribution of the value, for the envelope and the surface of the
    // page: fifteen quantiles from the sort the PFE already does.
    inputs.quantile_levels = {0.01, 0.025, 0.05, 0.10, 0.20, 0.30, 0.40, 0.50,
                              0.60, 0.70, 0.80, 0.90, 0.95, 0.975, 0.99};

    qm_::ExposureSimulationSettings settings;
    settings.paths = paths;
    settings.seed = seed;
    // blueprint/wp/23-xva.md §14.11: CPU, GPU, or GPU when the trades allow it.
    if (device == "gpu")
        settings.device = qm_::ComputeDevice::Gpu;
    else if (device == "auto")
        settings.device = qm_::ComputeDevice::Auto;
    else if (device != "cpu")
        throw qm_::InvalidInput("xva: unknown device '" + device + "' (cpu, gpu, auto)");
    if (!csa.is_none())
    {
        const py::dict c = csa.cast<py::dict>();
        const qm_::Csa terms = csa_terms(c);
        inputs.collateral.cashflows = margin_period_cashflows(c);
        inputs.csa = terms;
        settings.grid.margin_period_of_risk = terms.margin_period_of_risk;
        settings.keep_cashflows = inputs.collateral.cashflows != qm_::MarginPeriodCashflows::Paid;
    }

    inputs.collateral_spread = collateral_spread;
    inputs.wrong_way_b = wrong_way_b;
    if (!initial_margin.is_none())
    {
        // Initial margin both ways, projected by regression; the clean move
        // of the value needs the trades' cash flows.
        const py::dict m = initial_margin.cast<py::dict>();
        qm_::DimSettings dim;
        dim.confidence = item_or<qm_::Real>(m, "confidence", 0.99);
        if (m.contains("im_today") && !m["im_today"].is_none())
            dim.im_today = m["im_today"].cast<qm_::Real>();
        inputs.initial_margin = dim;
        inputs.initial_margin_spread = item_or<qm_::Real>(m, "spread", borrowing_spread);
        settings.keep_cashflows = true;
        // Where the margin comes from: the regression's own figure, the
        // regression started from today's SIMM, or SIMM on every path.
        const auto margin_model = item_or<std::string>(m, "model", "regression");
        if (margin_model == "simm")
            inputs.margin_model = qm_::XvaInputs::MarginModel::SimmPerPath;
        else if (margin_model == "regression_on_simm")
            inputs.margin_model = qm_::XvaInputs::MarginModel::RegressionOnSimm;
        else if (margin_model != "regression")
            throw qm_::InvalidInput("xva: unknown initial margin model '" + margin_model +
                                    "' (regression, regression_on_simm, simm)");
        settings.simm = inputs.margin_model != qm_::XvaInputs::MarginModel::Regression;
    }
    if (!capital.is_none())
    {
        const py::dict k = capital.cast<py::dict>();
        qm_::CapitalInputs c;
        c.method = internal_model ? qm_::ExposureMethod::InternalModel : qm_::ExposureMethod::SaCcr;
        c.trades = regulatory_trades;
        c.pd = k["pd"].cast<qm_::Real>();
        c.lgd = item_or<qm_::Real>(k, "lgd", 0.40);
        c.large_financial = item_or<bool>(k, "large_financial", false);
        c.sector = ba_cva_sector(item_or<std::string>(k, "sector", "other"));
        c.quality = item_or<bool>(k, "investment_grade", false)
                        ? qm_::ba_cva::CreditQuality::InvestmentGrade
                        : qm_::ba_cva::CreditQuality::HighYieldOrNotRated;
        // SA-CCR is margined when the counterparty posts variation margin
        // (CRE52.2): a finite threshold on its side.
        if (inputs.csa && std::isfinite(inputs.csa->threshold_counterparty))
        {
            qm_::sa_ccr::MarginAgreement agreement;
            agreement.threshold = inputs.csa->threshold_counterparty;
            agreement.minimum_transfer_amount = inputs.csa->minimum_transfer_amount;
            agreement.nica = inputs.csa->independent_amount;
            agreement.margin_period_of_risk = inputs.csa->margin_period_of_risk;
            c.margin = agreement;
        }
        inputs.capital = c;
        inputs.cost_of_capital = item_or<qm_::Real>(k, "cost_of_capital", 0.10);
    }

    std::optional<qm_::XvaReport> report, uncollateralised;
    std::optional<qm_::ExposureStatistics> risk;
    std::vector<qm_::Real> values_today;
    std::size_t pilot_paths = 0;
    std::optional<qm_::simm::Margin> simm_today;
    std::string ran_on, device_note;
    int gpus = 0;
    {
        py::gil_scoped_release release;
        // The cube is shared and each path writes its own row: the worker
        // count changes the wall time, never the numbers.
        qm_::ThreadPool pool;
        const std::size_t workers =
            threads > 0 ? threads - 1 : std::min<std::size_t>(qm_::available_cpus(), 16) - 1;
        if (workers > 0)
            pool.start(workers);
        const qm_::ExposurePaths cube =
            two_currency_engine ? two_currency_engine->simulate(settings, workers > 0 ? &pool : nullptr)
                                : engine.simulate(settings, workers > 0 ? &pool : nullptr);
        values_today = cube.trade_values_today;
        pilot_paths = cube.pilot_paths;
        ran_on = cube.device;
        gpus = cube.gpus;
        device_note = cube.device_note;
        if (settings.simm)
            simm_today = cube.simm_today;
        report = qm_::xva_report(cube, inputs);
        if (inputs.csa)
        {
            // The same paths without the CSA: what the collateral is worth.
            qm_::XvaInputs open = inputs;
            open.csa.reset();
            open.initial_margin.reset();
            if (open.capital)
                open.capital->margin.reset();
            uncollateralised = qm_::xva_report(cube, open);
        }
        if (historical)
        {
            // The risk measures do not need the margin of every scenario.
            settings.simm = false;
            settings.historical =
                qm_::HistoricalRateDynamics{std::get<0>(*historical), std::get<1>(*historical), std::get<2>(*historical)};
            const qm_::ExposurePaths real_world = engine.simulate(settings, workers > 0 ? &pool : nullptr);
            risk = inputs.csa ? qm_::exposure_statistics(
                                    qm_::collateralise(real_world, *inputs.csa, {}, inputs.collateral), {},
                                    pfe_confidence)
                              : qm_::exposure_statistics(real_world, {}, pfe_confidence);
        }
    }

    py::dict out = report_dict(*report);
    out["trade_values_today"] = values_today;
    // Paths of the pilot that fitted the trades valued by regression (0: none).
    out["pilot_paths"] = pilot_paths;
    // Where the paths were valued, on how many cards, and why the CPU when
    // the GPU was allowed.
    out["device"] = ran_on;
    out["gpus"] = gpus;
    out["device_note"] = device_note;
    // Today's SIMM and its three parts, when the margin comes from it.
    if (simm_today)
    {
        py::dict margin;
        margin["delta"] = simm_today->delta;
        margin["vega"] = simm_today->vega;
        margin["curvature"] = simm_today->curvature;
        margin["total"] = simm_today->total();
        out["simm_today"] = margin;
    }
    else
        out["simm_today"] = py::none();
    out["uncollateralised"] = uncollateralised ? py::object(report_dict(*uncollateralised)) : py::object(py::none());
    out["risk"] = risk ? py::object(exposure_dict(*risk)) : py::object(py::none());
    return out;
}

namespace
{
    const char *const kXvaOutputNames[qm_::kXvaOutputs] = {"cva", "dva", "fca", "fba", "cva_unilateral"};

    py::list quote_risks(const std::vector<qm_::XvaQuoteRisk> &risks)
    {
        py::list out;
        for (const qm_::XvaQuoteRisk &q : risks)
        {
            py::dict d;
            d["label"] = q.label;
            d["expiry"] = q.expiry;
            d["tenor"] = q.tenor;
            d["level"] = q.level;
            for (std::size_t o = 0; o < qm_::kXvaOutputs; ++o)
                d[kXvaOutputNames[o]] = estimate_dict(q.risk[o]);
            out.append(d);
        }
        return out;
    }
} // namespace

static py::dict xva_sensitivities_impl(const std::vector<std::pair<qm_::Time, qm_::Real>> &swap_rates,
                                       const std::vector<std::tuple<qm_::Time, qm_::Time, qm_::Real>> &swaption_vols,
                                       const py::list &trades,
                                       const std::vector<std::pair<qm_::Time, qm_::Real>> &counterparty_spreads,
                                       const std::pair<std::vector<qm_::Time>, std::vector<qm_::Real>> &own_hazard,
                                       qm_::Real recovery, qm_::Real lgd_counterparty, qm_::Real lgd_own,
                                       const py::object &csa, qm_::Real borrowing_spread, qm_::Real lending_spread,
                                       std::size_t paths, std::uint64_t seed, std::size_t threads,
                                       const std::string &sector, bool investment_grade,
                                       qm_::Real exercise_smoothing, const py::object &foreign)
{
    // The market, rebuilt here from its quotes: the curve is their bootstrap,
    // a and σ the fit to the swaption vols, the counterparty's hazard rates
    // the bootstrap of its credit spreads.
    std::vector<qm_::ParRateQuote> par;
    for (const auto &[tenor, rate] : swap_rates)
        par.push_back(qm_::make_ois_quote(tenor, rate));
    const qm_::DiscountCurve curve = qm_::bootstrap_curve({}, par);
    qm_::XvaMarketQuotes quotes;
    quotes.swap_rates = swap_rates;
    for (const auto &[expiry, tenor, vol] : swaption_vols)
        quotes.swaption_vols.push_back({expiry, tenor, vol});
    quotes.recovery = recovery;
    const qm_::HullWhiteCalibration calibration = qm_::calibrate_hull_white(curve, curve, quotes.swaption_vols, 1, 1);
    const qm_::HullWhiteCurveModel model(calibration.mean_reversion, calibration.sigma, curve);

    std::vector<qm_::CdsQuote> cds;
    for (const auto &[tenor, spread] : counterparty_spreads)
    {
        cds.push_back({tenor, spread});
        quotes.counterparty_spread_tenors.push_back(tenor);
    }

    qm_::HullWhiteExposureEngine engine(model);
    std::vector<qm_::sa_ccr::Trade> regulatory_trades;
    // A second factor (blueprint/wp/23-xva.md §14.17): a second currency or
    // an asset, and the engine that simulates it with the rate. Its spot
    // volatility is the calibration to the market volatility of one option.
    std::optional<qm_::CrossCurrencyHullWhiteModel> two_factors;
    std::optional<qm_::CrossCurrencyExposureEngine> two_factor_engine;
    qm_::Time vol_expiry = 0.0;
    qm_::Real market_volatility = 0.0;
    bool asset = false;
    if (foreign.is_none())
        add_xva_trades(engine, trades, model, curve, true, regulatory_trades);
    else
    {
        const py::dict f = foreign.cast<py::dict>();
        asset = f.contains("dividend_yield");
        vol_expiry = f["volatility_expiry"].cast<qm_::Time>();
        market_volatility = f["market_volatility"].cast<qm_::Real>();
        const qm_::Real spot = f["spot"].cast<qm_::Real>();
        if (asset)
        {
            const qm_::Real rho = item_or<qm_::Real>(f, "rate_correlation", 0.0);
            const qm_::Real yield = f["dividend_yield"].cast<qm_::Real>();
            const qm_::HullWhiteCurveModel still(model.mean_reversion(), qm_::kDeterministicRateVolatility,
                                                 qm_::DiscountCurve(yield));
            const qm_::Real sigma_s =
                qm_::calibrated_fx_volatility(model, still, {0.0, rho, 0.0}, vol_expiry, market_volatility);
            two_factors.emplace(qm_::make_asset_model(model, spot, yield, sigma_s, rho));
        }
        else
        {
            const auto hw = f["hull_white"].cast<std::pair<qm_::Real, qm_::Real>>();
            const auto rho = f["correlations"].cast<std::tuple<qm_::Real, qm_::Real, qm_::Real>>();
            const qm_::HullWhiteCurveModel other(
                hw.first, hw.second,
                rate_curve(f["discount_times"].cast<std::vector<qm_::Time>>(),
                           f["discount_factors"].cast<std::vector<qm_::Real>>()));
            const qm_::CrossCurrencyHullWhiteModel::Correlations correlations{std::get<0>(rho), std::get<1>(rho),
                                                                              std::get<2>(rho)};
            const qm_::Real sigma_s =
                qm_::calibrated_fx_volatility(model, other, correlations, vol_expiry, market_volatility);
            two_factors.emplace(model, other, spot, sigma_s, correlations);
        }
        two_factor_engine.emplace(*two_factors);
        const auto currency = item_or<std::string>(f, "currency", asset ? "asset" : "EUR");
        add_two_currency_trades(*two_factor_engine, trades, *two_factors,
                                currency + "/" + item_or<std::string>(f, "domestic_currency", "USD"),
                                currency, regulatory_trades, asset, item_or<bool>(f, "index", false));
    }

    qm_::XvaRiskInputs inputs;
    inputs.counterparty = qm_::bootstrap_credit_curve(cds, curve, recovery);
    inputs.own = credit_curve(own_hazard);
    inputs.lgd_counterparty = lgd_counterparty;
    inputs.lgd_own = lgd_own;
    inputs.borrowing_spread = borrowing_spread;
    inputs.lending_spread = lending_spread;
    inputs.exercise_smoothing = exercise_smoothing;
    if (!csa.is_none())
    {
        const py::dict c = csa.cast<py::dict>();
        inputs.csa = csa_terms(c);
        inputs.cashflows = margin_period_cashflows(c);
    }
    qm_::ExposureSimulationSettings settings;
    settings.paths = paths;
    settings.seed = seed;
    const qm_::ba_cva::Sector bucket = ba_cva_sector(sector);
    const qm_::ba_cva::CreditQuality quality = investment_grade
                                                   ? qm_::ba_cva::CreditQuality::InvestmentGrade
                                                   : qm_::ba_cva::CreditQuality::HighYieldOrNotRated;

    std::optional<qm_::XvaRisks> risks;
    std::optional<qm_::XvaValues> values;
    std::optional<qm_::XvaMarketRisks> market;
    std::size_t workers = 0;
    {
        py::gil_scoped_release release;
        // A path's tape is a couple of megabytes: past eight threads they no
        // longer fit the cache together and more threads are slower
        // (blueprint/wp/23-xva.md §14.13).
        qm_::ThreadPool pool;
        workers = (threads > 0 ? threads : std::min<std::size_t>(qm_::available_cpus(), 8)) - 1;
        if (workers > 0)
            pool.start(workers);
        qm_::ThreadPool *p = workers > 0 ? &pool : nullptr;
        risks = two_factor_engine ? two_factor_engine->xva_risks(settings, inputs, p)
                                  : engine.xva_risks(settings, inputs, p);
        // The same estimator without its sensitivities: what the adjoint
        // costs is measured against it.
        qm_::XvaRiskInputs same = inputs;
        same.smoothing_widths = risks->smoothing_widths;
        values = two_factor_engine ? two_factor_engine->xva_values(settings, same, p)
                                   : engine.xva_values(settings, same, p);
        // At a fixed market volatility of the spot, a and σ of the rates
        // move its volatility in the model with them: folded in before the
        // rate model's own inputs are mapped to their quotes.
        if (two_factors)
            qm_::fold_spot_volatility_calibration(*risks, *two_factors, vol_expiry, market_volatility);
        market = qm_::xva_market_risks(*risks, model, inputs.counterparty, inputs.own, quotes);
    }

    py::dict out;
    py::dict adjustments;
    for (std::size_t o = 0; o < qm_::kXvaOutputs; ++o)
        adjustments[kXvaOutputNames[o]] = estimate_dict(risks->values[o]);
    out["adjustments"] = adjustments;
    py::dict hw;
    hw["mean_reversion"] = calibration.mean_reversion;
    hw["sigma"] = calibration.sigma;
    out["hull_white"] = hw;

    py::list model_risks;
    for (std::size_t j = 0; j < risks->factors.size(); ++j)
    {
        py::dict d;
        d["label"] = risks->labels[j];
        d["tenor"] = risks->tenors[j];
        d["level"] = risks->levels[j];
        for (std::size_t o = 0; o < qm_::kXvaOutputs; ++o)
            d[kXvaOutputNames[o]] = estimate_dict(risks->risk(static_cast<qm_::XvaOutput>(o), j));
        model_risks.append(d);
    }
    out["model_risks"] = model_risks;
    out["swap_rates"] = quote_risks(market->swap_rates);
    out["swaption_vols"] = quote_risks(market->swaption_vols);
    out["counterparty_spreads"] = quote_risks(market->counterparty_spreads);
    out["own_credit"] = quote_risks(market->own_spreads);
    out["others"] = quote_risks(market->others);

    // What the second factor is read on, or estimated: its spot, its market
    // volatility, its curve (a dividend yield, or the foreign zero rates),
    // the foreign rate model and the correlations. Reported as they are.
    if (two_factors)
    {
        std::vector<qm_::XvaQuoteRisk> second;
        for (std::size_t j = 0; j < risks->factors.size(); ++j)
        {
            const qm_::XvaRiskFactor factor = risks->factors[j];
            const bool of_the_asset = factor == qm_::XvaRiskFactor::Spot ||
                                      factor == qm_::XvaRiskFactor::SpotVolatility ||
                                      factor == qm_::XvaRiskFactor::ForeignZeroRate ||
                                      factor == qm_::XvaRiskFactor::CorrelationDomesticSpot;
            const bool of_the_currency = factor == qm_::XvaRiskFactor::ForeignMeanReversion ||
                                         factor == qm_::XvaRiskFactor::ForeignSigma ||
                                         factor == qm_::XvaRiskFactor::CorrelationDomesticForeign ||
                                         factor == qm_::XvaRiskFactor::CorrelationForeignSpot;
            // A yield that does not move has no rate model of its own.
            if (of_the_asset || (of_the_currency && !asset))
                second.push_back(qm_::factor_risk(*risks, j));
        }
        out["second_factor"] = quote_risks(second);
        out["spot_volatility"] = two_factors->fx_volatility();
        out["sa_cva"] = py::none();
        out["paths"] = risks->paths;
        out["threads"] = workers + 1;
        out["factors"] = risks->factors.size();
        out["seconds_adjoint"] = risks->seconds;
        out["seconds_valuation"] = values->seconds;
        out["smoothing_widths"] = risks->smoothing_widths;
        return out;
    }
    out["second_factor"] = py::list();

    const qm_::sa_cva::Sensitivities sensitivities = qm_::sa_cva_sensitivities(*market, bucket, quality);
    const qm_::sa_cva::Capital k = qm_::sa_cva::capital(sensitivities);
    py::dict sa;
    sa["interest_rate_tenors"] =
        std::vector<qm_::Time>(qm_::sa_cva::interest_rate_tenors.begin(), qm_::sa_cva::interest_rate_tenors.end());
    sa["interest_rate_delta"] =
        std::vector<qm_::Real>(sensitivities.interest_rate_delta.begin(), sensitivities.interest_rate_delta.end());
    sa["interest_rate_risk_weights"] = std::vector<qm_::Real>(qm_::sa_cva::interest_rate_risk_weights.begin(),
                                                              qm_::sa_cva::interest_rate_risk_weights.end());
    sa["interest_rate_vega"] = sensitivities.interest_rate_vega;
    sa["credit_spread_tenors"] =
        std::vector<qm_::Time>(qm_::sa_cva::credit_spread_tenors.begin(), qm_::sa_cva::credit_spread_tenors.end());
    sa["credit_spread_delta"] =
        std::vector<qm_::Real>(sensitivities.credit_spread_delta.begin(), sensitivities.credit_spread_delta.end());
    sa["credit_spread_risk_weight"] = qm_::sa_cva::credit_spread_risk_weight(bucket, quality);
    sa["capital_interest_rate_delta"] = k.interest_rate_delta;
    sa["capital_interest_rate_vega"] = k.interest_rate_vega;
    sa["capital_credit_spread_delta"] = k.credit_spread_delta;
    sa["capital"] = k.total();
    out["sa_cva"] = sa;

    out["paths"] = risks->paths;
    out["threads"] = workers + 1;
    out["factors"] = risks->factors.size();
    out["seconds_adjoint"] = risks->seconds;
    out["seconds_valuation"] = values->seconds;
    out["smoothing_widths"] = risks->smoothing_widths;
    return out;
}

static py::dict estimate_historical_rate_dynamics_impl(const std::vector<qm_::Real> &rates, qm_::Time dt)
{
    const qm_::HistoricalRateEstimate e = qm_::estimate_historical_rate_dynamics(rates, dt);
    py::dict out;
    out["mean_reversion"] = e.dynamics.mean_reversion;
    out["long_run_rate"] = e.dynamics.long_run_rate;
    out["sigma"] = e.dynamics.sigma;
    out["half_life"] = e.dynamics.half_life();
    out["mean_reversion_std_error"] = e.mean_reversion_std_error;
    out["long_run_rate_std_error"] = e.long_run_rate_std_error;
    out["sigma_std_error"] = e.sigma_std_error;
    out["observations"] = e.observations;
    return out;
}

void register_xva(py::module_ &m)
{
    m.def("cross_currency_fx_volatility", &cross_currency_fx_volatility_impl,
          py::arg("domestic_hull_white"), py::arg("foreign_hull_white"), py::arg("correlations"),
          py::arg("expiry"), py::arg("market_volatility"), py::arg("expiries") = std::vector<qm_::Time>{},
          "Spot volatility of the two-currency Hull-White model (rates (a, sigma) in each currency, "
          "correlations (domestic-foreign, domestic-fx, foreign-fx)) that gives an FX option of this "
          "expiry its market Black volatility, and the model's Black volatilities at `expiries`.");
    m.def("xva_netting_set", &xva_netting_set_impl, py::arg("discount_times"), py::arg("discount_factors"),
          py::arg("hull_white"), py::arg("trades"), py::arg("counterparty_hazard"), py::arg("own_hazard"),
          py::arg("lgd_counterparty") = 0.6, py::arg("lgd_own") = 0.6, py::arg("csa") = py::none(),
          py::arg("borrowing_spread") = 0.0, py::arg("lending_spread") = 0.0,
          py::arg("historical") = py::none(), py::arg("paths") = 10000, py::arg("seed") = 42,
          py::arg("pfe_confidence") = 0.95, py::arg("threads") = 0,
          py::arg("initial_margin") = py::none(), py::arg("collateral_spread") = 0.0,
          py::arg("capital") = py::none(), py::arg("wrong_way_b") = 0.0, py::arg("device") = "cpu",
          py::arg("foreign") = py::none(),
          "Exposure and CVA / DVA of a netting set of swaps, European and Bermudan swaptions under "
          "Hull-White (a, sigma): trades are dicts (kind 'swap' | 'swaption' | 'bermudan', tenor, "
          "fixed_rate, notional, payer, "
          "quantity, start | expiry, frequencies; or kind 'script' with script, valuation_date, "
          "quantity: a rates-only payoff script); hazard curves are (times, hazards); csa a dict of its "
          "terms or None; historical (a, theta, sigma) adds the risk-measure profiles. initial_margin "
          "(a dict: confidence, im_today, spread, model 'regression' | 'regression_on_simm' | 'simm') "
          "projects the margin both parties post and gives the MVA — by regression, or as ISDA SIMM "
          "from the sensitivities of each path (swaps and European swaptions only); "
          "collateral_spread (what the CSA pays over the discount rate) the ColVA; capital (a dict: pd, lgd, "
          "sector, investment_grade, large_financial, cost_of_capital, method 'sa_ccr' | "
          "'internal_model') the IRB and BA-CVA capital and the KVA, on an exposure at default "
          "that is SA-CCR's or, with 'internal_model', 1.4 x the Effective EPE of the simulation "
          "(CRE53), the only one a scripted trade has. wrong_way_b makes the counterparty's hazard exp(a(t) + b V(t)) "
          "(Hull & White 2012), per unit of currency; cva_independent is then the CVA without it. "
          "device 'cpu' | 'gpu' | 'auto' is where the paths are valued: the GPU takes swaps and "
          "European swaptions without SIMM per path; 'auto' falls back to the CPU and says why in "
          "device_note. foreign (a dict: discount_times, discount_factors, hull_white (a, sigma), spot in "
          "domestic units per foreign unit, fx_volatility, correlations (domestic-foreign, domestic-fx, "
          "foreign-fx), currency, domestic_currency) adds a second currency: trades are then swaps "
          "and swaptions with currency 'domestic' | 'foreign', kind 'fx_forward' (strike, maturity, "
          "notional in foreign currency) and kind 'cross_currency_swap' (maturity, foreign_notional, "
          "domestic_notional, foreign_rate, domestic_rate, frequency, receive_foreign); everything is "
          "in domestic currency, on the CPU, without the historical measure or SIMM. "
          "foreign with dividend_yield (and spot, volatility, rate_correlation, currency = its name, "
          "index) is instead an asset with a known yield, lognormal under the Hull-White rate: the "
          "trades on it are kind 'asset_option' (strike, expiry, is_call, notional in units), European "
          "and cash-settled, next to domestic rate trades. "
          "A cost is negative.");
    m.def("xva_sensitivities", &xva_sensitivities_impl, py::arg("swap_rates"), py::arg("swaption_vols"),
          py::arg("trades"), py::arg("counterparty_spreads"), py::arg("own_hazard"), py::arg("recovery") = 0.4,
          py::arg("lgd_counterparty") = 0.6, py::arg("lgd_own") = 0.6, py::arg("csa") = py::none(),
          py::arg("borrowing_spread") = 0.0, py::arg("lending_spread") = 0.0, py::arg("paths") = 5000,
          py::arg("seed") = 42, py::arg("threads") = 0, py::arg("sector") = "other",
          py::arg("investment_grade") = false, py::arg("exercise_smoothing") = 0.05,
          py::arg("foreign") = py::none(),
          "Sensitivities of CVA, DVA, FCA and FBA of a netting set of swaps and European swaptions to every "
          "market quote, by adjoint differentiation (blueprint/wp/23-xva.md, lot X8). The market is rebuilt "
          "from its quotes: swap_rates [(tenor, par rate)] are bootstrapped into the curve, swaption_vols "
          "[(expiry, tenor, normal vol)] calibrate Hull-White, counterparty_spreads [(tenor, spread)] "
          "bootstrap the counterparty's hazard rates. Returns the adjustments, the risks to the model's "
          "inputs and to each quote with their Monte-Carlo errors, the SA-CVA capital (MAR50) from the "
          "sensitivities of the unilateral CVA, and the time of the adjoint run against one valuation. "
          "foreign (as in xva_netting_set, with market_volatility and volatility_expiry instead of the "
          "spot's own volatility, which is calibrated to them) adds a second currency or an asset: the "
          "risks of the rate model's inputs are then at a fixed market volatility, second_factor holds "
          "those of the spot, its market volatility, its curve, the foreign rate model and the "
          "correlations, and sa_cva is None (it would need the FX or equity risk classes).");
    m.def("estimate_historical_rate_dynamics", &estimate_historical_rate_dynamics_impl, py::arg("rates"),
          py::arg("dt"),
          "Maximum-likelihood (a, theta, sigma) of a mean-reverting Gaussian short rate from a series "
          "observed every dt years, with standard errors. Raises when no mean reversion is measurable.");
}
