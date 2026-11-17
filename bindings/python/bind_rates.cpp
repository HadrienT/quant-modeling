// Python bindings, rates (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/engines/analytic/cds.hpp"
#include "quantModeling/market/credit_bootstrap.hpp"
#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/utils/thread_pool.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/models/credit/merton_structural.hpp"
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

// ── Discount-curve bootstrap (market/curve_bootstrap.hpp) ─────────────────────
//
// For the rates page: a government par-yield curve (Treasury CMT, JGB) becomes a
// discount curve with the desk algorithm already in the library, rather than a
// second implementation in Python. Rates are decimals (0.0397), times in years.
// Returns the pillar times and their discount factors; discount_factors() then
// queries that curve with DiscountCurve's own interpolation (log-linear in the
// discount factor), so derived zero and forward rates use the exact rule the
// bootstrap solved under.

static py::dict bootstrap_discount_curve_impl(
    const std::vector<std::pair<quantModeling::Time, quantModeling::Real>> &deposits,
    const std::vector<std::pair<quantModeling::Time, quantModeling::Real>> &semiannual_par)
{
    std::vector<quantModeling::DepositQuote> dep;
    std::vector<quantModeling::ParRateQuote> par;
    std::vector<quantModeling::Time> pillars;
    for (const auto &[t, r] : deposits)
    {
        dep.push_back({t, r});
        pillars.push_back(t);
    }
    for (const auto &[t, y] : semiannual_par)
    {
        par.push_back(quantModeling::make_semiannual_bond_quote(t, y));
        pillars.push_back(t);
    }
    const quantModeling::DiscountCurve curve = quantModeling::bootstrap_curve(dep, par);
    std::sort(pillars.begin(), pillars.end());
    std::vector<quantModeling::Real> dfs;
    dfs.reserve(pillars.size());
    for (const quantModeling::Time t : pillars)
        dfs.push_back(curve.discount(t));
    py::dict out;
    out["times"] = pillars;
    out["discount_factors"] = dfs;
    return out;
}

static std::vector<quantModeling::Real> discount_factors_impl(
    std::vector<quantModeling::Time> times, std::vector<quantModeling::Real> dfs,
    const std::vector<quantModeling::Time> &query_times)
{
    const quantModeling::DiscountCurve curve(std::move(times), std::move(dfs));
    std::vector<quantModeling::Real> out;
    out.reserve(query_times.size());
    for (const quantModeling::Time t : query_times)
        out.push_back(curve.discount(t));
    return out;
}

// ── Credit (market/credit_curve.hpp, credit_bootstrap.hpp, engines/analytic/cds.hpp,
//    models/credit/merton_structural.hpp) ─────────────────────────────────────
//
// For the credit page. A discount curve crosses the boundary as its pillars
// (times, discount factors) — what bootstrap_discount_curve returns — and a
// credit curve as (times, hazards). Spreads and rates are decimals.

namespace
{
    quantModeling::DiscountCurve discount_from(std::vector<quantModeling::Time> times,
                                               std::vector<quantModeling::Real> dfs)
    {
        if (times.empty())
            return quantModeling::DiscountCurve(0.0);
        return quantModeling::DiscountCurve(std::move(times), std::move(dfs));
    }
} // namespace

static py::dict bootstrap_credit_curve_impl(
    const std::vector<std::pair<quantModeling::Time, quantModeling::Real>> &spreads,
    std::vector<quantModeling::Time> discount_times, std::vector<quantModeling::Real> discount_dfs,
    quantModeling::Real recovery, int frequency)
{
    std::vector<quantModeling::CdsQuote> quotes;
    for (const auto &[t, s] : spreads)
        quotes.push_back({t, s});
    const auto curve = quantModeling::bootstrap_credit_curve(
        quotes, discount_from(std::move(discount_times), std::move(discount_dfs)), recovery,
        frequency);
    py::dict out;
    out["times"] = curve.times();
    out["hazards"] = curve.hazards();
    return out;
}

static std::vector<quantModeling::Real> survival_probabilities_impl(
    std::vector<quantModeling::Time> times, std::vector<quantModeling::Real> hazards,
    const std::vector<quantModeling::Time> &query_times)
{
    const quantModeling::CreditCurve curve(std::move(times), std::move(hazards));
    std::vector<quantModeling::Real> out;
    out.reserve(query_times.size());
    for (const quantModeling::Time t : query_times)
        out.push_back(curve.survival(t));
    return out;
}

static py::dict cds_legs_impl(quantModeling::Time maturity, quantModeling::Real spread,
                              std::vector<quantModeling::Time> hazard_times,
                              std::vector<quantModeling::Real> hazards,
                              std::vector<quantModeling::Time> discount_times,
                              std::vector<quantModeling::Real> discount_dfs,
                              quantModeling::Real recovery, int frequency)
{
    const auto cds = quantModeling::make_cds(maturity, spread, frequency);
    const auto legs = quantModeling::cds_legs(
        cds, discount_from(std::move(discount_times), std::move(discount_dfs)),
        quantModeling::CreditCurve(std::move(hazard_times), std::move(hazards)), recovery);
    py::dict out;
    out["risky_annuity"] = legs.risky_annuity;
    out["accrued_on_default"] = legs.accrued_on_default;
    out["protection"] = legs.protection;
    out["par_spread"] = legs.par_spread();
    out["npv"] = quantModeling::cds_npv(cds, legs);
    return out;
}

static py::dict merton_calibrate_impl(quantModeling::Real equity_value,
                                      quantModeling::Real equity_vol,
                                      quantModeling::Real debt_face, quantModeling::Real rate,
                                      quantModeling::Time horizon)
{
    const auto c =
        quantModeling::calibrate_merton(equity_value, equity_vol, debt_face, rate, horizon);
    py::dict out;
    out["asset_value"] = c.firm.asset_value;
    out["asset_vol"] = c.firm.asset_vol;
    out["iterations"] = c.iterations;
    out["converged"] = c.converged;
    out["relative_residual"] = c.relative_residual;
    return out;
}

static py::dict merton_term_structure_impl(quantModeling::Real asset_value,
                                           quantModeling::Real asset_vol,
                                           quantModeling::Real debt_face, quantModeling::Real rate,
                                           const std::vector<quantModeling::Time> &maturities)
{
    const quantModeling::MertonFirm firm{asset_value, asset_vol, debt_face, rate};
    std::vector<quantModeling::Real> spread, pd, dd, recovery;
    for (const quantModeling::Time T : maturities)
    {
        spread.push_back(firm.credit_spread(T));
        pd.push_back(firm.default_probability(T));
        dd.push_back(firm.distance_to_default(T));
        recovery.push_back(firm.expected_recovery(T));
    }
    py::dict out;
    out["maturities"] = maturities;
    out["credit_spread"] = spread;
    out["default_probability"] = pd;
    out["distance_to_default"] = dd;
    out["expected_recovery"] = recovery;
    return out;
}

// ── Rates (market/multi_curve_bootstrap.hpp, engines/analytic/swap.hpp,
//    hull_white_swaption.hpp, market/hull_white_calibration.hpp) ──────────────
//
// For the rates page (blueprint/wp/21-rates.md). Curves cross the boundary as
// their pillars (times, discount factors) and are rebuilt here with a flat
// forward outside them (CurveExtrapolation::FlatForward); inside, the
// interpolation is the bootstrap's own. Rates and vols are decimals.

namespace
{
    namespace qm_ = quantModeling;

    py::dict curve_dict(const qm_::DiscountCurve &c)
    {
        py::dict out;
        out["times"] = c.pillar_times();
        out["discount_factors"] = c.pillar_discount_factors();
        return out;
    }
} // namespace

static py::dict bootstrap_ois_curve_impl(const std::vector<std::pair<qm_::Time, qm_::Real>> &deposits,
                                         const std::vector<std::pair<qm_::Time, qm_::Real>> &ois)
{
    std::vector<qm_::DepositQuote> dep;
    std::vector<qm_::ParRateQuote> par;
    for (const auto &[t, r] : deposits)
        dep.push_back({t, r});
    for (const auto &[t, r] : ois)
        par.push_back(qm_::make_ois_quote(t, r));
    return curve_dict(qm_::bootstrap_curve(dep, par));
}

static py::dict bootstrap_projection_curve_impl(
    std::vector<qm_::Time> discount_times, std::vector<qm_::Real> discount_dfs,
    const std::vector<std::tuple<qm_::Time, qm_::Time, qm_::Real>> &fras,
    const std::vector<std::pair<qm_::Time, qm_::Real>> &swaps, int fixed_frequency, int float_frequency)
{
    const qm_::DiscountCurve disc = rate_curve(std::move(discount_times), std::move(discount_dfs));
    std::vector<qm_::FraQuote> f;
    for (const auto &[s, e, r] : fras)
        f.push_back({s, e, r, e - s});
    std::vector<qm_::ProjectionSwapQuote> q;
    for (const auto &[t, r] : swaps)
        q.push_back(qm_::make_projection_swap_quote(t, r, fixed_frequency, float_frequency));
    return curve_dict(qm_::bootstrap_projection_curve(disc, f, q));
}

static std::vector<qm_::Real> rate_curve_discount_factors_impl(std::vector<qm_::Time> times,
                                                               std::vector<qm_::Real> dfs,
                                                               const std::vector<qm_::Time> &query_times)
{
    const qm_::DiscountCurve c = rate_curve(std::move(times), std::move(dfs));
    std::vector<qm_::Real> out;
    for (const qm_::Time t : query_times)
        out.push_back(c.discount(t));
    return out;
}

static py::dict price_swap_impl(std::vector<qm_::Time> dt, std::vector<qm_::Real> dd, std::vector<qm_::Time> pt,
                                std::vector<qm_::Real> pd, qm_::Time start, qm_::Time tenor, qm_::Real fixed_rate,
                                int fixed_frequency, int float_frequency, qm_::Real notional, bool payer,
                                qm_::Real spread)
{
    const qm_::DiscountCurve disc = rate_curve(std::move(dt), std::move(dd));
    const qm_::DiscountCurve proj = rate_curve(std::move(pt), std::move(pd));
    const auto swap =
        qm_::make_swap(start, tenor, fixed_rate, fixed_frequency, float_frequency, notional, payer, spread);
    const auto v = qm_::value_swap(swap, qm_::MultiCurve{disc, proj});
    py::dict out;
    out["npv"] = v.npv;
    out["fixed_leg"] = v.fixed_leg;
    out["floating_leg"] = v.floating_leg;
    out["annuity"] = v.annuity;
    out["par_rate"] = v.par_rate;
    // PV01: the value of 1bp on the fixed rate, per the notional.
    out["pv01"] = v.annuity * notional * 1e-4;
    py::list fixed, floating;
    for (const auto &c : swap.fixed_leg)
        fixed.append(py::make_tuple(c.start, c.end, c.payment, c.accrual, disc.discount(c.payment)));
    for (const auto &c : swap.floating_leg)
        floating.append(py::make_tuple(c.start, c.end, c.payment, c.accrual, disc.discount(c.payment),
                                       qm_::forward_rate(proj, c.start, c.end, c.accrual)));
    out["fixed_periods"] = fixed;
    out["floating_periods"] = floating;
    return out;
}

static py::dict price_swaption_impl(std::vector<qm_::Time> dt, std::vector<qm_::Real> dd, std::vector<qm_::Time> pt,
                                    std::vector<qm_::Real> pd, qm_::Time expiry, qm_::Time tenor,
                                    std::optional<qm_::Real> strike, bool payer, int fixed_frequency,
                                    int float_frequency, qm_::Real notional, std::optional<qm_::Real> normal_vol,
                                    std::optional<qm_::Real> lognormal_vol, qm_::Real shift,
                                    std::optional<std::tuple<qm_::Real, qm_::Real, qm_::Real, qm_::Real>> sabr,
                                    std::optional<std::pair<qm_::Real, qm_::Real>> hull_white,
                                    const std::vector<qm_::Time> &bermudan_exercises)
{
    const qm_::DiscountCurve disc = rate_curve(std::move(dt), std::move(dd));
    const qm_::DiscountCurve proj = rate_curve(std::move(pt), std::move(pd));
    qm_::Swaption swpt(qm_::make_swap(expiry, tenor, 0.0, fixed_frequency, float_frequency, notional, payer), expiry);
    const auto f = qm_::swaption_forward(swpt, qm_::MultiCurve{disc, proj});
    const qm_::Real K = strike.value_or(f.forward);
    swpt.swap.fixed_rate = K;
    const qm_::Real A = f.annuity * notional;
    py::dict out;
    out["forward"] = f.forward;
    out["annuity"] = f.annuity;
    out["strike"] = K;
    if (normal_vol)
        out["bachelier"] = qm_::bachelier_swaption(payer, f.forward, K, expiry, *normal_vol, A);
    if (lognormal_vol)
        out["black"] = qm_::black_swaption(payer, f.forward, K, expiry, *lognormal_vol, A, shift);
    if (sabr)
    {
        const auto &[alpha, beta, rho, nu] = *sabr;
        const qm_::SABRParams p{alpha, beta, rho, nu};
        out["sabr"] = qm_::sabr_swaption(payer, f.forward, K, expiry, p, A, shift);
        out["sabr_lognormal_vol"] = qm_::sabr_implied_vol(f.forward + shift, K + shift, expiry, p);
    }
    if (hull_white)
    {
        const qm_::HullWhiteCurveModel m(hull_white->first, hull_white->second, disc, proj);
        const qm_::Real price = qm_::hull_white_european_swaption(swpt, m);
        out["hull_white"] = price;
        out["hull_white_normal_vol"] = qm_::bachelier_implied_vol(payer, price, f.forward, K, expiry, A);
        if (!bermudan_exercises.empty())
            out["hull_white_bermudan"] =
                qm_::hull_white_bermudan_swaption(qm_::BermudanSwaption(swpt.swap, bermudan_exercises), m);
    }
    return out;
}

static py::dict calibrate_hull_white_impl(std::vector<qm_::Time> dt, std::vector<qm_::Real> dd,
                                          std::vector<qm_::Time> pt, std::vector<qm_::Real> pd,
                                          const std::vector<std::tuple<qm_::Time, qm_::Time, qm_::Real>> &quotes,
                                          int fixed_frequency, int float_frequency,
                                          std::optional<qm_::Real> fixed_mean_reversion)
{
    const qm_::DiscountCurve disc = rate_curve(std::move(dt), std::move(dd));
    const qm_::DiscountCurve proj = rate_curve(std::move(pt), std::move(pd));
    std::vector<qm_::SwaptionVolQuote> q;
    for (const auto &[e, t, v] : quotes)
        q.push_back({e, t, v});
    const auto c = qm_::calibrate_hull_white(disc, proj, q, fixed_frequency, float_frequency, fixed_mean_reversion);
    py::dict out;
    out["mean_reversion"] = c.mean_reversion;
    out["sigma"] = c.sigma;
    out["rmse_bp"] = c.report.rmse;
    out["worst_bp"] = c.report.worst_residual;
    out["iterations"] = c.report.iterations;
    out["converged"] = c.report.converged;
    out["seconds"] = c.report.wall_time_seconds;
    out["strikes"] = c.strikes;
    out["market_vols"] = c.market_vols;
    out["model_vols"] = c.model_vols;
    return out;
}

void register_rates(py::module_ &m)
{
    m.def("bootstrap_discount_curve", &bootstrap_discount_curve_impl,
          py::arg("deposits"), py::arg("semiannual_par"),
          "Bootstrap a discount curve from money-market deposits [(maturity, simple rate)] "
          "and semi-annual par yields [(maturity, par yield)] (rates as decimals, times in "
          "years; par maturities whole half-years). Returns {'times', 'discount_factors'} at "
          "every pillar. Raises RuntimeError (InvalidInput) on duplicate maturities or an "
          "unbracketable quote.");
    m.def("discount_factors", &discount_factors_impl, py::arg("times"),
          py::arg("discount_factors"), py::arg("query_times"),
          "Discount factors at query_times on the curve (times, discount_factors), with "
          "DiscountCurve's log-linear interpolation (flat before the first pillar and "
          "after the last).");
    m.def("bootstrap_credit_curve", &bootstrap_credit_curve_impl, py::arg("spreads"),
          py::arg("discount_times"), py::arg("discount_factors"), py::arg("recovery") = 0.4,
          py::arg("frequency") = 4,
          "Bootstrap a piecewise-constant hazard curve from par CDS spreads [(maturity, spread)] "
          "on the discount curve (discount_times, discount_factors) — empty lists mean a zero "
          "rate. Returns {'times', 'hazards'}. Raises RuntimeError (InvalidInput) when a spread "
          "curve falls too steeply to be reached by a non-negative hazard.");
    m.def("survival_probabilities", &survival_probabilities_impl, py::arg("times"),
          py::arg("hazards"), py::arg("query_times"),
          "Survival probabilities at query_times on the piecewise-constant hazard curve "
          "(times, hazards).");
    m.def("cds_legs", &cds_legs_impl, py::arg("maturity"), py::arg("spread"),
          py::arg("hazard_times"), py::arg("hazards"), py::arg("discount_times"),
          py::arg("discount_factors"), py::arg("recovery") = 0.4, py::arg("frequency") = 4,
          "Both legs of a protection buyer's CDS per unit notional: risky_annuity (incl. "
          "accrued_on_default), protection, par_spread and npv.");
    m.def("merton_calibrate", &merton_calibrate_impl, py::arg("equity_value"),
          py::arg("equity_vol"), py::arg("debt_face"), py::arg("rate"),
          py::arg("horizon") = 1.0,
          "Merton (1974) structural model: the asset value and asset volatility that reproduce "
          "the observed equity value and equity volatility for a zero-coupon debt of face "
          "debt_face at `horizon`.");
    m.def("merton_term_structure", &merton_term_structure_impl, py::arg("asset_value"),
          py::arg("asset_vol"), py::arg("debt_face"), py::arg("rate"), py::arg("maturities"),
          "Risk-neutral Merton credit spread, default probability, distance to default and "
          "expected recovery at each maturity.");
    m.def("bootstrap_ois_curve", &bootstrap_ois_curve_impl, py::arg("deposits"), py::arg("ois"),
          "Bootstrap the OIS discount curve from deposits [(maturity, simple rate)] and par OIS "
          "swaps [(maturity, rate)] (annual coupons). Returns {'times', 'discount_factors'}.");
    m.def("bootstrap_projection_curve", &bootstrap_projection_curve_impl, py::arg("discount_times"),
          py::arg("discount_factors"), py::arg("fras"), py::arg("swaps"), py::arg("fixed_frequency") = 1,
          py::arg("float_frequency") = 4,
          "Bootstrap a floating index's projection curve from FRAs [(start, end, rate)] and par swaps "
          "[(tenor, rate)] discounted on the given OIS curve (multi-curve). Returns {'times', "
          "'discount_factors'} of the pseudo-discount curve.");
    m.def("rate_curve_discount_factors", &rate_curve_discount_factors_impl, py::arg("times"),
          py::arg("discount_factors"), py::arg("query_times"),
          "Discount factors on a rates curve: log-linear between pillars, flat forward outside.");
    m.def("price_swap", &price_swap_impl, py::arg("discount_times"), py::arg("discount_factors"),
          py::arg("projection_times"), py::arg("projection_factors"), py::arg("start"), py::arg("tenor"),
          py::arg("fixed_rate"), py::arg("fixed_frequency") = 1, py::arg("float_frequency") = 4,
          py::arg("notional") = 1.0, py::arg("payer") = true, py::arg("spread") = 0.0,
          "Multi-curve value of a vanilla swap: npv, legs, annuity, par rate, PV01 and the periods.");
    m.def("price_swaption", &price_swaption_impl, py::arg("discount_times"), py::arg("discount_factors"),
          py::arg("projection_times"), py::arg("projection_factors"), py::arg("expiry"), py::arg("tenor"),
          py::arg("strike") = py::none(), py::arg("payer") = true, py::arg("fixed_frequency") = 1,
          py::arg("float_frequency") = 4, py::arg("notional") = 1.0, py::arg("normal_vol") = py::none(),
          py::arg("lognormal_vol") = py::none(), py::arg("shift") = 0.0, py::arg("sabr") = py::none(),
          py::arg("hull_white") = py::none(), py::arg("bermudan_exercises") = std::vector<qm_::Time>{},
          "A European swaption on the multi-curve forward (strike None = ATM): Bachelier, (shifted) "
          "Black, shifted SABR (alpha, beta, rho, nu) and Hull-White (a, sigma) prices, whichever are "
          "given; with Hull-White and bermudan_exercises, also the Bermudan on the same swap.");
    m.def("calibrate_hull_white", &calibrate_hull_white_impl, py::arg("discount_times"),
          py::arg("discount_factors"), py::arg("projection_times"), py::arg("projection_factors"),
          py::arg("quotes"), py::arg("fixed_frequency") = 1, py::arg("float_frequency") = 4,
          py::arg("fixed_mean_reversion") = py::none(),
          "Calibrate Hull-White (a, sigma) to ATM swaption normal vols [(expiry, tenor, vol)]; the "
          "report reads in bp of normal vol.");
}
