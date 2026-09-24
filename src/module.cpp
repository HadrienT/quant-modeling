#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/engines/mc/local_vol.hpp"
#include "quantModeling/engines/mc/path_simulation.hpp"
#include "quantModeling/market/sabr_calibration.hpp"

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/equity/simulatable_asian.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/calendars.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/market/conventions.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/market/vol_surface_pipeline.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/scripting/model_advice.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace quantModeling
{

    static PricingResult price_vanilla_impl(const VanillaBSInput &in, bool use_mc)
    {
        const EngineKind engine = use_mc ? EngineKind::MonteCarlo : EngineKind::Analytic;
        PricingRequest request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_asian_impl(const AsianBSInput &in, bool use_mc)
    {
        const EngineKind engine = use_mc ? EngineKind::MonteCarlo : EngineKind::Analytic;
        PricingRequest request{
            InstrumentKind::EquityAsianOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_zero_coupon_impl(const ZeroCouponBondInput &in)
    {
        PricingRequest request{
            InstrumentKind::ZeroCouponBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_fixed_rate_impl(const FixedRateBondInput &in)
    {
        PricingRequest request{
            InstrumentKind::FixedRateBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_american_vanilla_impl(const AmericanVanillaBSInput &in, EngineKind engine)
    {
        PricingRequest request{
            InstrumentKind::EquityAmericanVanillaOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_vanilla_pde_impl(const VanillaBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            EngineKind::PDEFiniteDifference,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_barrier_impl(const BarrierBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBarrierOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_digital_impl(const DigitalBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityDigitalOption,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_lookback_impl(const LookbackBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityLookbackOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_basket_impl(const BasketBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBasketOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_barrier_lv_impl(const BarrierLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBarrierOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_lookback_lv_impl(const LookbackLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityLookbackOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_asian_lv_impl(const AsianLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityAsianOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Autocall ────────────────────────────────────────────────────────

    static PricingResult price_autocall_impl(const AutocallBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::Autocall,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Mountain (Himalaya) ─────────────────────────────────────────────

    static PricingResult price_mountain_impl(const MountainBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::Mountain,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Variance Swap ───────────────────────────────────────────────────

    static PricingResult price_variance_swap_analytic_impl(const VarianceSwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VarianceSwap,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_variance_swap_mc_impl(const VarianceSwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VarianceSwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Volatility Swap ─────────────────────────────────────────────────

    static PricingResult price_volatility_swap_mc_impl(const VolatilitySwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VolatilitySwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Dispersion Swap ─────────────────────────────────────────────────

    static PricingResult price_dispersion_mc_impl(const DispersionBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::DispersionSwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── FX Forward ──────────────────────────────────────────────────────

    static PricingResult price_fx_forward_impl(const FXForwardInput &in)
    {
        PricingRequest request{
            InstrumentKind::FXForward,
            ModelKind::GarmanKohlhagen,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── FX Option ───────────────────────────────────────────────────────

    static PricingResult price_fx_option_impl(const FXOptionInput &in)
    {
        PricingRequest request{
            InstrumentKind::FXOption,
            ModelKind::GarmanKohlhagen,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Commodity Forward ───────────────────────────────────────────────

    static PricingResult price_commodity_forward_impl(const CommodityForwardInput &in)
    {
        PricingRequest request{
            InstrumentKind::CommodityForward,
            ModelKind::CommodityBlack,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Commodity Option ────────────────────────────────────────────────

    static PricingResult price_commodity_option_impl(const CommodityOptionInput &in)
    {
        PricingRequest request{
            InstrumentKind::CommodityOption,
            ModelKind::CommodityBlack,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Worst-of Option ─────────────────────────────────────────────────

    static PricingResult price_worst_of_impl(const RainbowBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::WorstOfOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Best-of Option ──────────────────────────────────────────────────

    static PricingResult price_best_of_impl(const RainbowBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::BestOfOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

} // namespace quantModeling

static py::dict pricing_result_to_dict(const quantModeling::PricingResult &res)
{
    py::dict greeks;

    auto to_py = [](const auto &maybe_val) -> py::object
    {
        if (maybe_val)
            return py::float_(*maybe_val);
        return py::none();
    };

    greeks["delta"] = to_py(res.greeks.delta);
    greeks["gamma"] = to_py(res.greeks.gamma);
    greeks["vega"] = to_py(res.greeks.vega);
    greeks["theta"] = to_py(res.greeks.theta);
    greeks["rho"] = to_py(res.greeks.rho);

    greeks["delta_std_error"] = to_py(res.greeks.delta_std_error);
    greeks["gamma_std_error"] = to_py(res.greeks.gamma_std_error);
    greeks["vega_std_error"] = to_py(res.greeks.vega_std_error);
    greeks["theta_std_error"] = to_py(res.greeks.theta_std_error);
    greeks["rho_std_error"] = to_py(res.greeks.rho_std_error);

    py::dict out;
    out["npv"] = static_cast<double>(res.npv);
    out["greeks"] = greeks;
    out["diagnostics"] = res.diagnostics;
    out["mc_std_error"] = static_cast<double>(res.mc_std_error);

    // Bond analytics (optional fields — only populated for bond instruments)
    py::dict bond_analytics;
    bond_analytics["macaulay_duration"] = to_py(res.bond_analytics.macaulay_duration);
    bond_analytics["modified_duration"] = to_py(res.bond_analytics.modified_duration);
    bond_analytics["convexity"] = to_py(res.bond_analytics.convexity);
    bond_analytics["dv01"] = to_py(res.bond_analytics.dv01);
    out["bond_analytics"] = bond_analytics;

    // Per-parameter AAD sensitivities (blueprint/wp/17-aad.md §13.1) --
    // absent (None) for anything not priced with greeks_method="aad".
    if (res.risks)
    {
        py::list risks;
        for (std::size_t i = 0; i < res.risks->labels.size(); ++i)
        {
            py::dict entry;
            entry["label"] = res.risks->labels[i];
            entry["value"] = static_cast<double>(res.risks->values[i]);
            entry["std_error"] = static_cast<double>(res.risks->std_errors[i]);
            risks.append(entry);
        }
        out["risks"] = risks;
    }
    else
    {
        out["risks"] = py::none();
    }

    return out;
}

static py::dict price_vanilla_bs_analytic(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_impl(in, false);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_mc(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_impl(in, true);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_pde(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_pde_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_binomial(const quantModeling::VanillaBSInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityVanillaOption,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::BinomialTree,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_trinomial(const quantModeling::VanillaBSInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityVanillaOption,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::TrinomialTree,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_american_vanilla_bs_binomial(const quantModeling::AmericanVanillaBSInput &in)
{
    auto res = quantModeling::price_american_vanilla_impl(in, quantModeling::EngineKind::BinomialTree);
    return pricing_result_to_dict(res);
}

static py::dict price_american_vanilla_bs_trinomial(const quantModeling::AmericanVanillaBSInput &in)
{
    auto res = quantModeling::price_american_vanilla_impl(in, quantModeling::EngineKind::TrinomialTree);
    return pricing_result_to_dict(res);
}

static py::dict price_asian_bs_analytic(const quantModeling::AsianBSInput &in)
{
    auto res = quantModeling::price_asian_impl(in, false);
    return pricing_result_to_dict(res);
}

static py::dict price_asian_bs_mc(const quantModeling::AsianBSInput &in)
{
    auto res = quantModeling::price_asian_impl(in, true);
    return pricing_result_to_dict(res);
}

static py::dict price_future_bs_analytic(const quantModeling::EquityFutureInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityFuture,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::Analytic,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_zero_coupon_bond_analytic(const quantModeling::ZeroCouponBondInput &in)
{
    auto res = quantModeling::price_zero_coupon_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_fixed_rate_bond_analytic(const quantModeling::FixedRateBondInput &in)
{
    auto res = quantModeling::price_fixed_rate_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_barrier_bs_mc(const quantModeling::BarrierBSInput &in)
{
    auto res = quantModeling::price_barrier_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_digital_bs_analytic(const quantModeling::DigitalBSInput &in)
{
    auto res = quantModeling::price_digital_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_lookback_bs_mc(const quantModeling::LookbackBSInput &in)
{
    auto res = quantModeling::price_lookback_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_basket_bs_mc(const quantModeling::BasketBSInput &in)
{
    auto res = quantModeling::price_basket_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_local_vol_mc_impl(const quantModeling::LocalVolInput &in)
{
    auto res = quantModeling::price_local_vol_mc(in);
    return pricing_result_to_dict(res);
}

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

// ── Vol surface calibration: raw quotes -> SVI per maturity -> Dupire grid ──
//
// Replaces api/app/local_vol/{fetcher,cleaner,iv_surface,dupire,cpp_bridge}.py
// and routers/market_iv_surface.py's independent yfinance/griddata path: one
// C++ pipeline, called from Python with the raw quotes it already has (from
// data-ingest's Postgres or a live yfinance fetch), returning a grid in
// exactly the K_grid/T_grid/sigma_loc_flat shape LocalVolInput above expects.

static py::dict calibrate_vol_surface_impl(
    std::vector<quantModeling::RawOptionQuote> quotes,
    quantModeling::Real spot, quantModeling::Real rate, quantModeling::Real dividend,
    quantModeling::Real k_min, quantModeling::Real k_max,
    std::size_t n_strikes, std::size_t n_maturities, std::size_t min_quotes_per_slice,
    const quantModeling::CleaningParams &cleaning_params)
{
    const auto result = quantModeling::calibrate_vol_surface(
        std::move(quotes), spot, rate, dividend, k_min, k_max, n_strikes, n_maturities,
        min_quotes_per_slice, cleaning_params);

    py::list slices;
    for (const auto &s : result.slices)
    {
        py::dict sd;
        sd["ttm"] = static_cast<double>(s.ttm);
        sd["a"] = static_cast<double>(s.params.a);
        sd["b"] = static_cast<double>(s.params.b);
        sd["rho"] = static_cast<double>(s.params.rho);
        sd["m"] = static_cast<double>(s.params.m);
        sd["sigma"] = static_cast<double>(s.params.sigma);
        sd["rmse"] = static_cast<double>(s.rmse);
        sd["worst_residual"] = static_cast<double>(s.worst_residual);
        sd["n_quotes"] = s.n_quotes;
        sd["iterations"] = s.iterations;
        sd["converged"] = s.converged;
        sd["butterfly_arbitrage_free"] = s.butterfly_arbitrage_free;
        slices.append(sd);
    }

    py::dict cleaning_stats;
    cleaning_stats["raw_count"] = result.cleaning_stats.raw_count;
    cleaning_stats["after_liquidity"] = result.cleaning_stats.after_liquidity;
    cleaning_stats["after_moneyness"] = result.cleaning_stats.after_moneyness;
    cleaning_stats["after_calendar_arbitrage"] = result.cleaning_stats.after_calendar_arbitrage;
    cleaning_stats["after_butterfly_arbitrage"] = result.cleaning_stats.after_butterfly_arbitrage;
    cleaning_stats["final_count"] = result.cleaning_stats.final_count;

    py::dict out;
    out["cleaning_stats"] = cleaning_stats;
    out["slices"] = slices;
    out["calendar_arbitrage_free"] = result.calendar_arbitrage_free;
    out["K_grid"] = result.K_grid;
    out["T_grid"] = result.T_grid;
    out["sigma_loc_flat"] = result.sigma_loc;
    // The log-moneyness range actually used for the grid above, after
    // clamping to what every slice observed (see VolSurfacePipelineResult::
    // k_min in vol_surface_pipeline.hpp) -- any other display grid built
    // from `slices` should reuse this rather than guessing its own range.
    out["k_min"] = static_cast<double>(result.k_min);
    out["k_max"] = static_cast<double>(result.k_max);
    return out;
}

// ── SABR calibration ─────────────────────────────────────────────────────────

static py::dict calibrate_sabr_slice_impl(
    std::vector<quantModeling::SABRSliceQuote> quotes,
    quantModeling::Real forward, quantModeling::Real ttm, quantModeling::Real beta)
{
    const auto result = quantModeling::calibrate_sabr_slice(std::move(quotes), forward, ttm, beta);

    py::dict out;
    out["ttm"] = static_cast<double>(result.ttm);
    out["alpha"] = static_cast<double>(result.params.alpha);
    out["beta"] = static_cast<double>(result.params.beta);
    out["rho"] = static_cast<double>(result.params.rho);
    out["nu"] = static_cast<double>(result.params.nu);
    out["rmse"] = static_cast<double>(result.report.rmse);
    out["worst_residual"] = static_cast<double>(result.report.worst_residual);
    out["iterations"] = result.report.iterations;
    out["converged"] = result.report.converged;
    return out;
}

// ── Path simulation (display/illustration, not a pricing engine) ───────────

static py::dict simulate_black_scholes_paths_impl(
    quantModeling::Real spot, quantModeling::Real rate, quantModeling::Real dividend,
    quantModeling::Real vol, quantModeling::Real ttm,
    std::size_t n_steps, long long n_paths, int seed)
{
    quantModeling::PathSimulationSettings settings;
    settings.n_steps = n_steps;
    settings.n_paths = n_paths;
    settings.seed = static_cast<std::uint64_t>(seed > 0 ? seed : 1);

    const auto result = quantModeling::simulate_black_scholes_paths(spot, rate, dividend, vol, ttm, settings);
    py::dict out;
    out["time_grid"] = result.time_grid;
    out["paths"] = result.paths;
    return out;
}

static py::dict simulate_sabr_paths_impl(
    quantModeling::Real forward, quantModeling::Real alpha, quantModeling::Real beta,
    quantModeling::Real rho, quantModeling::Real nu, quantModeling::Real ttm,
    std::size_t n_steps, long long n_paths, int seed)
{
    const quantModeling::SABRParams params{alpha, beta, rho, nu};
    quantModeling::PathSimulationSettings settings;
    settings.n_steps = n_steps;
    settings.n_paths = n_paths;
    settings.seed = static_cast<std::uint64_t>(seed > 0 ? seed : 1);

    const auto result = quantModeling::simulate_sabr_paths(forward, params, ttm, settings);
    py::dict out;
    out["time_grid"] = result.time_grid;
    out["paths"] = result.paths;
    return out;
}

// ── Dated Asian: the timeline / calendar architecture, end to end ──────────
//
// Fixings arrive as ISO-8601 date strings and are resolved to year-fractions
// against a valuation date and a day-count basis, then priced by the generic
// SimulationMCEngine + BlackScholesSimModel. No registry, no PricingInput
// variant — this is the new architecture, wired directly.

static const quantModeling::DayCounter &day_counter_by_name(const std::string &name)
{
    using namespace quantModeling;
    if (name == "ACT/360")
        return Actual360::instance();
    if (name == "30/360")
        return Thirty360::instance();
    if (name == "ACT/ACT")
        return ActualActualISDA::instance();
    if (name == "ACT/365F" || name.empty())
        return Actual365Fixed::instance();
    throw std::invalid_argument("unknown day-count basis: " + name);
}

static py::dict price_dated_asian(double spot, double rate, double dividend,
                                  double vol, const std::string &valuation_date,
                                  const std::vector<std::string> &fixing_dates,
                                  double strike, bool is_call, bool geometric,
                                  const std::string &day_count, int n_paths,
                                  int seed, const std::string &sampler)
{
    using namespace quantModeling;

    const Date valuation = Date::from_iso(valuation_date);
    const DayCounter &basis = day_counter_by_name(day_count);
    const ValuationContext ctx{valuation, &basis, &NullCalendar::instance()};

    std::vector<Time> fixings;
    fixings.reserve(fixing_dates.size());
    for (const std::string &iso : fixing_dates)
    {
        const Date d = Date::from_iso(iso);
        if (!(valuation < d))
            throw std::invalid_argument("fixing date " + iso +
                                        " must fall after the valuation date");
        fixings.push_back(ctx.t(d));
    }

    SimulatableAsian<Real> product(std::move(fixings), strike, is_call, geometric);
    BlackScholesSimModel<Real> model(spot, rate, dividend, vol);

    PricingSettings settings;
    settings.mc_paths = n_paths > 0 ? n_paths : 200000;
    settings.mc_seed = seed > 0 ? seed : 1;
    settings.mc_antithetic = true;
    settings.mc_sampler =
        (sampler == "sobol") ? SamplerKind::Sobol : SamplerKind::PseudoRandom;

    const SimulationMCResult mc = simulate<Real>(product, model, settings);

    PricingResult res;
    res.npv = mc.npv();
    res.mc_std_error = mc.std_error();
    res.diagnostics = mc.diagnostics + " | " +
                      std::to_string(fixing_dates.size()) + " fixings, basis " +
                      basis.name();
    return pricing_result_to_dict(res);
}

// ── Scripted product: WP 16 — a payoff described in text, priced by the same
//    generic Monte-Carlo engine as every other ISimulatableProduct. A parse
//    error (ScriptError) propagates as a Python RuntimeError carrying the
//    pointed line/column message; the API layer turns that into a 422.
//
// The script never says which dynamics its price depends on -- the model is
// a separate choice, made here:
//   model = "black_scholes": one flat vol (`vol`).
//   model = "local_vol":     a Dupire surface (K_grid / T_grid /
//                            sigma_loc_flat, the shape calibrate_vol_surface
//                            returns), simulated by Euler steps of
//                            1/steps_per_year.
// Whatever the choice, `warnings` reports what the script's price depends on
// that the chosen model cannot capture (scripting/model_advice.hpp) --
// structural facts read off the script, never a guess at the size of the
// error.
//
// greeks_method == "aad" reprices with T = aad::Number instead of Real
// (blueprint/wp/17-aad.md §7): every model parameter's sensitivity comes
// back in `risks`, at roughly 3-5x the cost of one price rather than 2N+1
// bumped reprices -- for local_vol that is one local vega per surface point.
// Sobol is not offered under AAD yet (skip_to for the adjoint engine is lot
// 17d) -- requesting it there falls back to pseudo-random and says so in
// `diagnostics` rather than silently ignoring it.
static py::list advice_to_py(const std::vector<quantModeling::scripting::Advice> &adv)
{
    py::list out;
    for (const auto &a : adv)
    {
        py::dict d;
        d["code"] = a.code;
        d["severity"] = a.severity;
        d["message"] = a.message;
        out.append(d);
    }
    return out;
}

static py::dict price_script(const std::string &script, double spot, double rate,
                             double dividend, double vol,
                             const std::string &valuation_date,
                             const std::string &day_count, bool fuzzy,
                             double default_eps, int n_paths, int seed,
                             const std::string &sampler,
                             const std::string &greeks_method,
                             const std::string &model,
                             const std::vector<double> &K_grid,
                             const std::vector<double> &T_grid,
                             const std::vector<double> &sigma_loc_flat,
                             int steps_per_year)
{
    using namespace quantModeling;

    const Date valuation = Date::from_iso(valuation_date);
    const DayCounter &basis = day_counter_by_name(day_count);
    const ValuationContext ctx{valuation, &basis, &NullCalendar::instance()};
    const int paths = n_paths > 0 ? n_paths : 200000;
    const std::uint64_t seed_value = static_cast<std::uint64_t>(seed > 0 ? seed : 1);
    if (steps_per_year < 1)
        throw std::invalid_argument("price_script: steps_per_year must be >= 1");
    const double max_dt = 1.0 / static_cast<double>(steps_per_year);

    const bool local_vol = (model == "local_vol");
    const scripting::ModelKind kind =
        local_vol ? scripting::ModelKind::LocalVolSurface
                  : scripting::ModelKind::BlackScholesFlatVol;
    const double surface_T = (local_vol && !T_grid.empty()) ? T_grid.back() : 0.0;
    const std::string model_note =
        local_vol ? " | model local_vol (" + std::to_string(K_grid.size()) + "x" +
                        std::to_string(T_grid.size()) + " surface, " +
                        std::to_string(steps_per_year) + " steps/yr)"
                  : " | model black_scholes (flat vol)";

    auto check_underlyings = [&](std::size_t needed, std::size_t available)
    {
        if (needed > available)
            throw std::invalid_argument(
                "price_script: the script reads spot(" + std::to_string(needed - 1) +
                ") but model '" + model + "' carries " +
                std::to_string(available) + " underlying(s)");
    };

    if (greeks_method == "aad")
    {
        ScriptedProduct<aad::Number> product(script, ctx,
                                             ScriptSettings{fuzzy, default_eps});
        auto sim_model = scripting::make_script_model<aad::Number>(
            model, spot, rate, dividend, vol, K_grid, T_grid, sigma_loc_flat,
            max_dt);
        check_underlyings(product.n_underlyings(), sim_model->n_underlyings());

        const AADSimulResults aad_res =
            simulate_aad(product, *sim_model, static_cast<std::size_t>(paths), seed_value);

        PricingResult res = to_pricing_result(aad_res);
        res.diagnostics += " | scripted, " +
                           std::to_string(product.timeline().size()) + " events" +
                           (fuzzy ? ", fuzzy" : ", hard") + model_note;
        if (sampler == "sobol")
            res.diagnostics += " | sobol requested but not available under AAD "
                               "yet (lot 17d) -- used pseudo-random instead";
        py::dict out = pricing_result_to_dict(res);
        out["warnings"] = advice_to_py(scripting::advise(
            product.analysis(), kind, product.timeline().back(), surface_T));
        return out;
    }

    ScriptedProduct<Real> product(script, ctx,
                                  ScriptSettings{fuzzy, default_eps});
    auto sim_model = scripting::make_script_model<Real>(model, spot, rate, dividend, vol,
                                                        K_grid, T_grid, sigma_loc_flat,
                                                        max_dt);
    check_underlyings(product.n_underlyings(), sim_model->n_underlyings());

    PricingSettings settings;
    settings.mc_paths = paths;
    settings.mc_seed = static_cast<int>(seed_value);
    settings.mc_antithetic = true;
    settings.mc_sampler =
        (sampler == "sobol") ? SamplerKind::Sobol : SamplerKind::PseudoRandom;

    const SimulationMCResult mc = simulate<Real>(product, *sim_model, settings);

    PricingResult res;
    res.npv = mc.npv();
    res.mc_std_error = mc.std_error();
    res.diagnostics = mc.diagnostics + " | scripted, " +
                      std::to_string(product.timeline().size()) + " events" +
                      (fuzzy ? ", fuzzy" : ", hard") + model_note;
    py::dict out = pricing_result_to_dict(res);
    out["warnings"] = advice_to_py(scripting::advise(
        product.analysis(), kind, product.timeline().back(), surface_T));
    return out;
}

// ── Validate a script without pricing it: parse + the pre-processing passes
//    only, no model, no Monte-Carlo. Lets a UI show the resolved timeline and
//    the detected variables (or the pointed parse error) instantly, before
//    committing to a full simulation.
static py::dict validate_script(const std::string &script,
                                const std::string &valuation_date,
                                const std::string &day_count)
{
    using namespace quantModeling;

    const Date valuation = Date::from_iso(valuation_date);
    const DayCounter &basis = day_counter_by_name(day_count);
    const ValuationContext ctx{valuation, &basis, &NullCalendar::instance()};

    const ScriptedProduct<Real> product(script, ctx);

    const std::vector<Date> dates = product.event_dates();
    const TimeLine &times = product.timeline();

    py::list events;
    for (std::size_t i = 0; i < dates.size(); ++i)
    {
        py::dict e;
        e["date"] = dates[i].to_iso();
        e["t"] = times[i];
        events.append(e);
    }

    py::list variables;
    for (const std::string &name : product.variable_names())
        variables.append(name);

    const scripting::ScriptAnalysis &a = product.analysis();
    py::dict analysis;
    analysis["n_underlyings"] = a.n_underlyings;
    analysis["nonlinear_in_spot"] = a.nonlinear_in_spot;
    analysis["spot_threshold_test"] = a.spot_threshold_test;
    analysis["path_dependent"] = a.path_dependent;

    py::dict out;
    out["events"] = events;
    out["variables"] = variables;
    out["analysis"] = analysis;
    return out;
}

#ifndef QM_BUILD_SHA
#define QM_BUILD_SHA "unknown"
#endif

PYBIND11_MODULE(quantmodeling, m)
{
    m.doc() = "quantModeling C++ bindings (pybind11)";

    m.def(
        "build_sha", []()
        { return std::string(QM_BUILD_SHA); },
        "Git short SHA the native module was built from (blueprint WP 18a "
        "section 5.1) -- read by the API's audit trail so a valuation can be "
        "traced to the exact lib_build that produced it.");

    py::enum_<quantModeling::AsianAverageType>(m, "AsianAverageType")
        .value("Arithmetic", quantModeling::AsianAverageType::Arithmetic)
        .value("Geometric", quantModeling::AsianAverageType::Geometric);

    py::enum_<quantModeling::BarrierType>(m, "BarrierType")
        .value("UpAndIn", quantModeling::BarrierType::UpAndIn)
        .value("UpAndOut", quantModeling::BarrierType::UpAndOut)
        .value("DownAndIn", quantModeling::BarrierType::DownAndIn)
        .value("DownAndOut", quantModeling::BarrierType::DownAndOut);

    py::enum_<quantModeling::DigitalPayoffType>(m, "DigitalPayoffType")
        .value("CashOrNothing", quantModeling::DigitalPayoffType::CashOrNothing)
        .value("AssetOrNothing", quantModeling::DigitalPayoffType::AssetOrNothing);

    py::enum_<quantModeling::LookbackStyle>(m, "LookbackStyle")
        .value("FixedStrike", quantModeling::LookbackStyle::FixedStrike)
        .value("FloatingStrike", quantModeling::LookbackStyle::FloatingStrike);

    py::enum_<quantModeling::LookbackExtremum>(m, "LookbackExtremum")
        .value("Minimum", quantModeling::LookbackExtremum::Minimum)
        .value("Maximum", quantModeling::LookbackExtremum::Maximum);

    py::class_<quantModeling::VanillaBSInput>(m, "VanillaBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::VanillaBSInput::spot)
        .def_readwrite("strike", &quantModeling::VanillaBSInput::strike)
        .def_readwrite("maturity", &quantModeling::VanillaBSInput::maturity)
        .def_readwrite("rate", &quantModeling::VanillaBSInput::rate)
        .def_readwrite("dividend", &quantModeling::VanillaBSInput::dividend)
        .def_readwrite("vol", &quantModeling::VanillaBSInput::vol)
        .def_readwrite("is_call", &quantModeling::VanillaBSInput::is_call)
        .def_readwrite("n_paths", &quantModeling::VanillaBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::VanillaBSInput::seed)
        .def_readwrite("mc_epsilon", &quantModeling::VanillaBSInput::mc_epsilon)
        .def_readwrite("tree_steps", &quantModeling::VanillaBSInput::tree_steps)
        .def_readwrite("pde_space_steps", &quantModeling::VanillaBSInput::pde_space_steps)
        .def_readwrite("pde_time_steps", &quantModeling::VanillaBSInput::pde_time_steps);

    py::class_<quantModeling::AmericanVanillaBSInput>(m, "AmericanVanillaBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::AmericanVanillaBSInput::spot)
        .def_readwrite("strike", &quantModeling::AmericanVanillaBSInput::strike)
        .def_readwrite("maturity", &quantModeling::AmericanVanillaBSInput::maturity)
        .def_readwrite("rate", &quantModeling::AmericanVanillaBSInput::rate)
        .def_readwrite("dividend", &quantModeling::AmericanVanillaBSInput::dividend)
        .def_readwrite("vol", &quantModeling::AmericanVanillaBSInput::vol)
        .def_readwrite("is_call", &quantModeling::AmericanVanillaBSInput::is_call)
        .def_readwrite("tree_steps", &quantModeling::AmericanVanillaBSInput::tree_steps)
        .def_readwrite("pde_space_steps", &quantModeling::AmericanVanillaBSInput::pde_space_steps)
        .def_readwrite("pde_time_steps", &quantModeling::AmericanVanillaBSInput::pde_time_steps);

    py::class_<quantModeling::AsianBSInput>(m, "AsianBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::AsianBSInput::spot)
        .def_readwrite("strike", &quantModeling::AsianBSInput::strike)
        .def_readwrite("maturity", &quantModeling::AsianBSInput::maturity)
        .def_readwrite("rate", &quantModeling::AsianBSInput::rate)
        .def_readwrite("dividend", &quantModeling::AsianBSInput::dividend)
        .def_readwrite("vol", &quantModeling::AsianBSInput::vol)
        .def_readwrite("is_call", &quantModeling::AsianBSInput::is_call)
        .def_readwrite("average_type", &quantModeling::AsianBSInput::average_type)
        .def_readwrite("n_paths", &quantModeling::AsianBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::AsianBSInput::seed)
        .def_readwrite("mc_epsilon", &quantModeling::AsianBSInput::mc_epsilon);

    py::class_<quantModeling::BarrierBSInput>(m, "BarrierBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::BarrierBSInput::spot)
        .def_readwrite("strike", &quantModeling::BarrierBSInput::strike)
        .def_readwrite("maturity", &quantModeling::BarrierBSInput::maturity)
        .def_readwrite("rate", &quantModeling::BarrierBSInput::rate)
        .def_readwrite("dividend", &quantModeling::BarrierBSInput::dividend)
        .def_readwrite("vol", &quantModeling::BarrierBSInput::vol)
        .def_readwrite("is_call", &quantModeling::BarrierBSInput::is_call)
        .def_readwrite("barrier_type", &quantModeling::BarrierBSInput::barrier_type)
        .def_readwrite("barrier_level", &quantModeling::BarrierBSInput::barrier_level)
        .def_readwrite("rebate", &quantModeling::BarrierBSInput::rebate)
        .def_readwrite("n_steps", &quantModeling::BarrierBSInput::n_steps)
        .def_readwrite("brownian_bridge", &quantModeling::BarrierBSInput::brownian_bridge)
        .def_readwrite("n_paths", &quantModeling::BarrierBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::BarrierBSInput::seed)
        .def_readwrite("mc_epsilon", &quantModeling::BarrierBSInput::mc_epsilon);

    py::class_<quantModeling::DigitalBSInput>(m, "DigitalBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::DigitalBSInput::spot)
        .def_readwrite("strike", &quantModeling::DigitalBSInput::strike)
        .def_readwrite("maturity", &quantModeling::DigitalBSInput::maturity)
        .def_readwrite("rate", &quantModeling::DigitalBSInput::rate)
        .def_readwrite("dividend", &quantModeling::DigitalBSInput::dividend)
        .def_readwrite("vol", &quantModeling::DigitalBSInput::vol)
        .def_readwrite("is_call", &quantModeling::DigitalBSInput::is_call)
        .def_readwrite("payoff_type", &quantModeling::DigitalBSInput::payoff_type)
        .def_readwrite("cash_amount", &quantModeling::DigitalBSInput::cash_amount);

    py::class_<quantModeling::LookbackBSInput>(m, "LookbackBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::LookbackBSInput::spot)
        .def_readwrite("strike", &quantModeling::LookbackBSInput::strike)
        .def_readwrite("maturity", &quantModeling::LookbackBSInput::maturity)
        .def_readwrite("rate", &quantModeling::LookbackBSInput::rate)
        .def_readwrite("dividend", &quantModeling::LookbackBSInput::dividend)
        .def_readwrite("vol", &quantModeling::LookbackBSInput::vol)
        .def_readwrite("is_call", &quantModeling::LookbackBSInput::is_call)
        .def_readwrite("style", &quantModeling::LookbackBSInput::style)
        .def_readwrite("extremum", &quantModeling::LookbackBSInput::extremum)
        .def_readwrite("n_steps", &quantModeling::LookbackBSInput::n_steps)
        .def_readwrite("n_paths", &quantModeling::LookbackBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::LookbackBSInput::seed)
        .def_readwrite("mc_antithetic", &quantModeling::LookbackBSInput::mc_antithetic)
        .def_readwrite("mc_epsilon", &quantModeling::LookbackBSInput::mc_epsilon);

    py::class_<quantModeling::BasketBSInput>(m, "BasketBSInput")
        .def(py::init<>())
        .def_readwrite("spots", &quantModeling::BasketBSInput::spots)
        .def_readwrite("vols", &quantModeling::BasketBSInput::vols)
        .def_readwrite("dividends", &quantModeling::BasketBSInput::dividends)
        .def_readwrite("weights", &quantModeling::BasketBSInput::weights)
        .def_readwrite("correlations", &quantModeling::BasketBSInput::correlations)
        .def_readwrite("strike", &quantModeling::BasketBSInput::strike)
        .def_readwrite("maturity", &quantModeling::BasketBSInput::maturity)
        .def_readwrite("rate", &quantModeling::BasketBSInput::rate)
        .def_readwrite("is_call", &quantModeling::BasketBSInput::is_call)
        .def_readwrite("n_paths", &quantModeling::BasketBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::BasketBSInput::seed)
        .def_readwrite("mc_antithetic", &quantModeling::BasketBSInput::mc_antithetic);

    py::class_<quantModeling::EquityFutureInput>(m, "EquityFutureInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::EquityFutureInput::spot)
        .def_readwrite("strike", &quantModeling::EquityFutureInput::strike)
        .def_readwrite("maturity", &quantModeling::EquityFutureInput::maturity)
        .def_readwrite("rate", &quantModeling::EquityFutureInput::rate)
        .def_readwrite("dividend", &quantModeling::EquityFutureInput::dividend)
        .def_readwrite("notional", &quantModeling::EquityFutureInput::notional);

    py::class_<quantModeling::ZeroCouponBondInput>(m, "ZeroCouponBondInput")
        .def(py::init<>())
        .def_readwrite("maturity", &quantModeling::ZeroCouponBondInput::maturity)
        .def_readwrite("rate", &quantModeling::ZeroCouponBondInput::rate)
        .def_readwrite("notional", &quantModeling::ZeroCouponBondInput::notional)
        .def_readwrite("discount_times", &quantModeling::ZeroCouponBondInput::discount_times)
        .def_readwrite("discount_factors", &quantModeling::ZeroCouponBondInput::discount_factors);

    py::class_<quantModeling::FixedRateBondInput>(m, "FixedRateBondInput")
        .def(py::init<>())
        .def_readwrite("maturity", &quantModeling::FixedRateBondInput::maturity)
        .def_readwrite("rate", &quantModeling::FixedRateBondInput::rate)
        .def_readwrite("coupon_rate", &quantModeling::FixedRateBondInput::coupon_rate)
        .def_readwrite("coupon_frequency", &quantModeling::FixedRateBondInput::coupon_frequency)
        .def_readwrite("notional", &quantModeling::FixedRateBondInput::notional)
        .def_readwrite("discount_times", &quantModeling::FixedRateBondInput::discount_times)
        .def_readwrite("discount_factors", &quantModeling::FixedRateBondInput::discount_factors);

    m.def("price_vanilla_bs_analytic", &price_vanilla_bs_analytic,
          "Price vanilla option under Black-Scholes (analytic).");
    m.def("price_vanilla_bs_mc", &price_vanilla_bs_mc,
          "Price vanilla option under Black-Scholes (Monte Carlo).");
    m.def("price_vanilla_bs_pde", &price_vanilla_bs_pde,
          "Price vanilla option under Black-Scholes (PDE Crank-Nicolson, European only).");
    m.def("price_vanilla_bs_binomial", &price_vanilla_bs_binomial,
          "Price European vanilla option under Black-Scholes (Binomial tree).");
    m.def("price_vanilla_bs_trinomial", &price_vanilla_bs_trinomial,
          "Price European vanilla option under Black-Scholes (Trinomial tree).");
    m.def("price_american_vanilla_bs_binomial", &price_american_vanilla_bs_binomial,
          "Price American vanilla option under Black-Scholes (Binomial tree).");
    m.def("price_american_vanilla_bs_trinomial", &price_american_vanilla_bs_trinomial,
          "Price American vanilla option under Black-Scholes (Trinomial tree).");
    m.def("price_asian_bs_analytic", &price_asian_bs_analytic,
          "Price Asian option under Black-Scholes (analytic).");
    m.def("price_asian_bs_mc", &price_asian_bs_mc,
          "Price Asian option under Black-Scholes (Monte Carlo).");
    m.def("price_dated_asian", &price_dated_asian, py::arg("spot"),
          py::arg("rate"), py::arg("dividend"), py::arg("vol"),
          py::arg("valuation_date"), py::arg("fixing_dates"), py::arg("strike"),
          py::arg("is_call"), py::arg("geometric"), py::arg("day_count"),
          py::arg("n_paths"), py::arg("seed"), py::arg("sampler"),
          "Price an average-price Asian from ISO-8601 fixing dates via the "
          "timeline simulation engine (calendar / day-count layer).");
    m.def("price_script", &price_script, py::arg("script"), py::arg("spot"),
          py::arg("rate"), py::arg("dividend"), py::arg("vol"),
          py::arg("valuation_date"), py::arg("day_count") = "ACT/365F",
          py::arg("fuzzy") = false, py::arg("default_eps") = 0.01,
          py::arg("n_paths") = 200000, py::arg("seed") = 1,
          py::arg("sampler") = "pseudo", py::arg("greeks_method") = "none",
          py::arg("model") = "black_scholes",
          py::arg("K_grid") = std::vector<double>{},
          py::arg("T_grid") = std::vector<double>{},
          py::arg("sigma_loc_flat") = std::vector<double>{},
          py::arg("steps_per_year") = 52,
          "Price a payoff script (blueprint/wp/16-scripting.md) via the "
          "timeline simulation engine. Raises on a malformed script, with "
          "the offending line and column in the message. greeks_method: "
          "'none' (default) or 'aad' -- every model parameter's sensitivity "
          "(blueprint/wp/17-aad.md), at roughly 3-5x one price's cost "
          "regardless of how many. 'bump' is not offered for scripted "
          "payoffs.");
    m.def("validate_script", &validate_script, py::arg("script"),
          py::arg("valuation_date"), py::arg("day_count") = "ACT/365F",
          "Parse a payoff script and resolve its timeline without pricing "
          "it. Raises the same way price_script does on a malformed script.");
    m.def("price_future_bs_analytic", &price_future_bs_analytic,
          "Price equity future under Black-Scholes (analytic).");
    m.def("price_zero_coupon_bond_analytic", &price_zero_coupon_bond_analytic,
          "Price zero coupon bond under flat-rate discounting (analytic).");
    m.def("price_fixed_rate_bond_analytic", &price_fixed_rate_bond_analytic,
          "Price fixed-rate bond under flat-rate discounting (analytic).");
    m.def("price_barrier_bs_mc", &price_barrier_bs_mc,
          "Price barrier option under Black-Scholes (Monte Carlo, all four barrier types).");
    m.def("price_digital_bs_analytic", &price_digital_bs_analytic,
          "Price digital option under Black-Scholes (analytic, Cash-or-Nothing / Asset-or-Nothing).");
    m.def("price_lookback_bs_mc", &price_lookback_bs_mc,
          "Price lookback option under Black-Scholes (Monte Carlo).");
    m.def("price_basket_bs_mc", &price_basket_bs_mc,
          "Price basket option under correlated multi-asset Black-Scholes (Monte Carlo).");

    py::class_<quantModeling::LocalVolInput>(m, "LocalVolInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::LocalVolInput::spot)
        .def_readwrite("strike", &quantModeling::LocalVolInput::strike)
        .def_readwrite("maturity", &quantModeling::LocalVolInput::maturity)
        .def_readwrite("rate", &quantModeling::LocalVolInput::rate)
        .def_readwrite("dividend", &quantModeling::LocalVolInput::dividend)
        .def_readwrite("is_call", &quantModeling::LocalVolInput::is_call)
        .def_readwrite("K_grid", &quantModeling::LocalVolInput::K_grid)
        .def_readwrite("T_grid", &quantModeling::LocalVolInput::T_grid)
        .def_readwrite("sigma_loc_flat", &quantModeling::LocalVolInput::sigma_loc_flat)
        .def_readwrite("n_paths", &quantModeling::LocalVolInput::n_paths)
        .def_readwrite("n_steps_per_year", &quantModeling::LocalVolInput::n_steps_per_year)
        .def_readwrite("seed", &quantModeling::LocalVolInput::seed)
        .def_readwrite("mc_antithetic", &quantModeling::LocalVolInput::mc_antithetic)
        .def_readwrite("compute_greeks", &quantModeling::LocalVolInput::compute_greeks);

    m.def("price_local_vol_mc", &price_local_vol_mc_impl,
          "Price a European vanilla option under a Dupire local-vol surface (C++ Euler-Maruyama MC).");

    // ── Vol surface calibration ──────────────────────────────────────────────────────
    py::class_<quantModeling::RawOptionQuote>(m, "RawOptionQuote")
        .def(py::init<>())
        .def_readwrite("strike", &quantModeling::RawOptionQuote::strike)
        .def_readwrite("ttm", &quantModeling::RawOptionQuote::ttm)
        .def_readwrite("is_call", &quantModeling::RawOptionQuote::is_call)
        .def_readwrite("bid", &quantModeling::RawOptionQuote::bid)
        .def_readwrite("ask", &quantModeling::RawOptionQuote::ask)
        .def_readwrite("last", &quantModeling::RawOptionQuote::last)
        .def_readwrite("volume", &quantModeling::RawOptionQuote::volume)
        .def_readwrite("open_interest", &quantModeling::RawOptionQuote::open_interest)
        .def_readwrite("implied_vol", &quantModeling::RawOptionQuote::implied_vol)
        .def_readwrite("has_iv", &quantModeling::RawOptionQuote::has_iv);

    py::class_<quantModeling::CleaningParams>(m, "CleaningParams")
        .def(py::init<>())
        .def_readwrite("min_open_interest", &quantModeling::CleaningParams::min_open_interest)
        .def_readwrite("min_bid", &quantModeling::CleaningParams::min_bid)
        .def_readwrite("max_spread_ratio", &quantModeling::CleaningParams::max_spread_ratio)
        .def_readwrite("min_moneyness", &quantModeling::CleaningParams::min_moneyness)
        .def_readwrite("max_moneyness", &quantModeling::CleaningParams::max_moneyness)
        .def_readwrite("oi_coverage_threshold", &quantModeling::CleaningParams::oi_coverage_threshold)
        .def_readwrite("min_plausible_iv", &quantModeling::CleaningParams::min_plausible_iv)
        .def_readwrite("max_plausible_iv", &quantModeling::CleaningParams::max_plausible_iv);

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
    m.def("calibrate_vol_surface", &calibrate_vol_surface_impl,
          py::arg("quotes"), py::arg("spot"), py::arg("rate"), py::arg("dividend"),
          py::arg("k_min") = -0.6, py::arg("k_max") = 0.6,
          py::arg("n_strikes") = 100, py::arg("n_maturities") = 50,
          py::arg("min_quotes_per_slice") = 6,
          py::arg("cleaning_params") = quantModeling::CleaningParams{},
          "Clean raw option quotes, calibrate one SVI slice per maturity, and build the "
          "Dupire local-vol grid (K_grid/T_grid/sigma_loc_flat) that price_local_vol_mc consumes. "
          "Raises RuntimeError (InvalidInput) if fewer than 2 maturities have enough clean quotes.");

    // ── LocalVolSurface sub-struct ─────────────────────────────────────────────────────────────
    py::class_<quantModeling::LocalVolSurface>(m, "LocalVolSurface")
        .def(py::init<>())
        .def_readwrite("K_grid", &quantModeling::LocalVolSurface::K_grid)
        .def_readwrite("T_grid", &quantModeling::LocalVolSurface::T_grid)
        .def_readwrite("sigma_loc_flat", &quantModeling::LocalVolSurface::sigma_loc_flat);

    // ── BarrierLocalVolInput ─────────────────────────────────────────────────────────────
    py::class_<quantModeling::BarrierLocalVolInput>(m, "BarrierLocalVolInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::BarrierLocalVolInput::spot)
        .def_readwrite("strike", &quantModeling::BarrierLocalVolInput::strike)
        .def_readwrite("maturity", &quantModeling::BarrierLocalVolInput::maturity)
        .def_readwrite("rate", &quantModeling::BarrierLocalVolInput::rate)
        .def_readwrite("dividend", &quantModeling::BarrierLocalVolInput::dividend)
        .def_readwrite("is_call", &quantModeling::BarrierLocalVolInput::is_call)
        .def_readwrite("barrier_type", &quantModeling::BarrierLocalVolInput::barrier_type)
        .def_readwrite("barrier_level", &quantModeling::BarrierLocalVolInput::barrier_level)
        .def_readwrite("rebate", &quantModeling::BarrierLocalVolInput::rebate)
        .def_readwrite("n_steps", &quantModeling::BarrierLocalVolInput::n_steps)
        .def_readwrite("brownian_bridge", &quantModeling::BarrierLocalVolInput::brownian_bridge)
        .def_readwrite("surface", &quantModeling::BarrierLocalVolInput::surface)
        .def_readwrite("n_paths", &quantModeling::BarrierLocalVolInput::n_paths)
        .def_readwrite("seed", &quantModeling::BarrierLocalVolInput::seed);

    m.def("price_barrier_lv_mc", [](const quantModeling::BarrierLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_barrier_lv_impl(in)); }, "Price a barrier option under a Dupire local-vol surface (Monte Carlo).");

    // ── LookbackLocalVolInput ────────────────────────────────────────────────────────────
    py::class_<quantModeling::LookbackLocalVolInput>(m, "LookbackLocalVolInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::LookbackLocalVolInput::spot)
        .def_readwrite("strike", &quantModeling::LookbackLocalVolInput::strike)
        .def_readwrite("maturity", &quantModeling::LookbackLocalVolInput::maturity)
        .def_readwrite("rate", &quantModeling::LookbackLocalVolInput::rate)
        .def_readwrite("dividend", &quantModeling::LookbackLocalVolInput::dividend)
        .def_readwrite("is_call", &quantModeling::LookbackLocalVolInput::is_call)
        .def_readwrite("style", &quantModeling::LookbackLocalVolInput::style)
        .def_readwrite("extremum", &quantModeling::LookbackLocalVolInput::extremum)
        .def_readwrite("n_steps", &quantModeling::LookbackLocalVolInput::n_steps)
        .def_readwrite("surface", &quantModeling::LookbackLocalVolInput::surface)
        .def_readwrite("n_paths", &quantModeling::LookbackLocalVolInput::n_paths)
        .def_readwrite("seed", &quantModeling::LookbackLocalVolInput::seed)
        .def_readwrite("mc_antithetic", &quantModeling::LookbackLocalVolInput::mc_antithetic);

    m.def("price_lookback_lv_mc", [](const quantModeling::LookbackLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_lookback_lv_impl(in)); }, "Price a lookback option under a Dupire local-vol surface (Monte Carlo).");

    // ── AsianLocalVolInput ─────────────────────────────────────────────────────────────────
    py::class_<quantModeling::AsianLocalVolInput>(m, "AsianLocalVolInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::AsianLocalVolInput::spot)
        .def_readwrite("strike", &quantModeling::AsianLocalVolInput::strike)
        .def_readwrite("maturity", &quantModeling::AsianLocalVolInput::maturity)
        .def_readwrite("rate", &quantModeling::AsianLocalVolInput::rate)
        .def_readwrite("dividend", &quantModeling::AsianLocalVolInput::dividend)
        .def_readwrite("is_call", &quantModeling::AsianLocalVolInput::is_call)
        .def_readwrite("average_type", &quantModeling::AsianLocalVolInput::average_type)
        .def_readwrite("surface", &quantModeling::AsianLocalVolInput::surface)
        .def_readwrite("n_paths", &quantModeling::AsianLocalVolInput::n_paths)
        .def_readwrite("seed", &quantModeling::AsianLocalVolInput::seed)
        .def_readwrite("mc_antithetic", &quantModeling::AsianLocalVolInput::mc_antithetic);

    m.def("price_asian_lv_mc", [](const quantModeling::AsianLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_asian_lv_impl(in)); }, "Price an Asian option under a Dupire local-vol surface (Monte Carlo).");

    // ── Autocall ───────────────────────────────────────────────────────────────────
    py::class_<quantModeling::AutocallBSInput>(m, "AutocallBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::AutocallBSInput::spot)
        .def_readwrite("rate", &quantModeling::AutocallBSInput::rate)
        .def_readwrite("dividend", &quantModeling::AutocallBSInput::dividend)
        .def_readwrite("vol", &quantModeling::AutocallBSInput::vol)
        .def_readwrite("observation_dates", &quantModeling::AutocallBSInput::observation_dates)
        .def_readwrite("autocall_barrier", &quantModeling::AutocallBSInput::autocall_barrier)
        .def_readwrite("coupon_barrier", &quantModeling::AutocallBSInput::coupon_barrier)
        .def_readwrite("put_barrier", &quantModeling::AutocallBSInput::put_barrier)
        .def_readwrite("coupon_rate", &quantModeling::AutocallBSInput::coupon_rate)
        .def_readwrite("notional", &quantModeling::AutocallBSInput::notional)
        .def_readwrite("memory_coupon", &quantModeling::AutocallBSInput::memory_coupon)
        .def_readwrite("ki_continuous", &quantModeling::AutocallBSInput::ki_continuous)
        .def_readwrite("n_paths", &quantModeling::AutocallBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::AutocallBSInput::seed);

    m.def("price_autocall_bs_mc", [](const quantModeling::AutocallBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_autocall_impl(in)); }, "Price an autocallable note under Black-Scholes (Monte Carlo).");

    // ── Mountain (Himalaya) ────────────────────────────────────────────────────────
    py::class_<quantModeling::MountainBSInput>(m, "MountainBSInput")
        .def(py::init<>())
        .def_readwrite("spots", &quantModeling::MountainBSInput::spots)
        .def_readwrite("vols", &quantModeling::MountainBSInput::vols)
        .def_readwrite("dividends", &quantModeling::MountainBSInput::dividends)
        .def_readwrite("correlations", &quantModeling::MountainBSInput::correlations)
        .def_readwrite("observation_dates", &quantModeling::MountainBSInput::observation_dates)
        .def_readwrite("strike", &quantModeling::MountainBSInput::strike)
        .def_readwrite("is_call", &quantModeling::MountainBSInput::is_call)
        .def_readwrite("rate", &quantModeling::MountainBSInput::rate)
        .def_readwrite("notional", &quantModeling::MountainBSInput::notional)
        .def_readwrite("n_paths", &quantModeling::MountainBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::MountainBSInput::seed);

    m.def("price_mountain_bs_mc", [](const quantModeling::MountainBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_mountain_impl(in)); }, "Price a Himalaya (Mountain) option under multi-asset BS (Monte Carlo).");

    // ── Variance Swap ──────────────────────────────────────────────────────────────
    py::class_<quantModeling::VarianceSwapBSInput>(m, "VarianceSwapBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::VarianceSwapBSInput::spot)
        .def_readwrite("rate", &quantModeling::VarianceSwapBSInput::rate)
        .def_readwrite("dividend", &quantModeling::VarianceSwapBSInput::dividend)
        .def_readwrite("vol", &quantModeling::VarianceSwapBSInput::vol)
        .def_readwrite("maturity", &quantModeling::VarianceSwapBSInput::maturity)
        .def_readwrite("strike_var", &quantModeling::VarianceSwapBSInput::strike_var)
        .def_readwrite("notional", &quantModeling::VarianceSwapBSInput::notional)
        .def_readwrite("observation_dates", &quantModeling::VarianceSwapBSInput::observation_dates)
        .def_readwrite("n_paths", &quantModeling::VarianceSwapBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::VarianceSwapBSInput::seed);

    m.def("price_variance_swap_bs_analytic", [](const quantModeling::VarianceSwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_variance_swap_analytic_impl(in)); }, "Price a variance swap under Black-Scholes (analytic).");

    m.def("price_variance_swap_bs_mc", [](const quantModeling::VarianceSwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_variance_swap_mc_impl(in)); }, "Price a variance swap under Black-Scholes (Monte Carlo).");

    // ── Volatility Swap ────────────────────────────────────────────────────────────
    py::class_<quantModeling::VolatilitySwapBSInput>(m, "VolatilitySwapBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::VolatilitySwapBSInput::spot)
        .def_readwrite("rate", &quantModeling::VolatilitySwapBSInput::rate)
        .def_readwrite("dividend", &quantModeling::VolatilitySwapBSInput::dividend)
        .def_readwrite("vol", &quantModeling::VolatilitySwapBSInput::vol)
        .def_readwrite("maturity", &quantModeling::VolatilitySwapBSInput::maturity)
        .def_readwrite("strike_vol", &quantModeling::VolatilitySwapBSInput::strike_vol)
        .def_readwrite("notional", &quantModeling::VolatilitySwapBSInput::notional)
        .def_readwrite("observation_dates", &quantModeling::VolatilitySwapBSInput::observation_dates)
        .def_readwrite("n_paths", &quantModeling::VolatilitySwapBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::VolatilitySwapBSInput::seed);

    m.def("price_volatility_swap_bs_mc", [](const quantModeling::VolatilitySwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_volatility_swap_mc_impl(in)); }, "Price a volatility swap under Black-Scholes (Monte Carlo).");

    // ── Dispersion Swap ────────────────────────────────────────────────────────────
    py::class_<quantModeling::DispersionBSInput>(m, "DispersionBSInput")
        .def(py::init<>())
        .def_readwrite("spots", &quantModeling::DispersionBSInput::spots)
        .def_readwrite("vols", &quantModeling::DispersionBSInput::vols)
        .def_readwrite("dividends", &quantModeling::DispersionBSInput::dividends)
        .def_readwrite("weights", &quantModeling::DispersionBSInput::weights)
        .def_readwrite("correlations", &quantModeling::DispersionBSInput::correlations)
        .def_readwrite("maturity", &quantModeling::DispersionBSInput::maturity)
        .def_readwrite("strike_spread", &quantModeling::DispersionBSInput::strike_spread)
        .def_readwrite("rate", &quantModeling::DispersionBSInput::rate)
        .def_readwrite("notional", &quantModeling::DispersionBSInput::notional)
        .def_readwrite("observation_dates", &quantModeling::DispersionBSInput::observation_dates)
        .def_readwrite("n_paths", &quantModeling::DispersionBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::DispersionBSInput::seed);

    m.def("price_dispersion_bs_mc", [](const quantModeling::DispersionBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_dispersion_mc_impl(in)); }, "Price a dispersion swap under multi-asset BS (Monte Carlo).");

    // ── FX Forward ─────────────────────────────────────────────────────────────────
    py::class_<quantModeling::FXForwardInput>(m, "FXForwardInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::FXForwardInput::spot)
        .def_readwrite("rate_domestic", &quantModeling::FXForwardInput::rate_domestic)
        .def_readwrite("rate_foreign", &quantModeling::FXForwardInput::rate_foreign)
        .def_readwrite("vol", &quantModeling::FXForwardInput::vol)
        .def_readwrite("strike", &quantModeling::FXForwardInput::strike)
        .def_readwrite("maturity", &quantModeling::FXForwardInput::maturity)
        .def_readwrite("notional", &quantModeling::FXForwardInput::notional);

    m.def("price_fx_forward_analytic", [](const quantModeling::FXForwardInput &in)
          { return pricing_result_to_dict(quantModeling::price_fx_forward_impl(in)); }, "Price an FX forward (analytic).");

    // ── FX Option ──────────────────────────────────────────────────────────────────
    py::class_<quantModeling::FXOptionInput>(m, "FXOptionInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::FXOptionInput::spot)
        .def_readwrite("rate_domestic", &quantModeling::FXOptionInput::rate_domestic)
        .def_readwrite("rate_foreign", &quantModeling::FXOptionInput::rate_foreign)
        .def_readwrite("vol", &quantModeling::FXOptionInput::vol)
        .def_readwrite("strike", &quantModeling::FXOptionInput::strike)
        .def_readwrite("maturity", &quantModeling::FXOptionInput::maturity)
        .def_readwrite("is_call", &quantModeling::FXOptionInput::is_call)
        .def_readwrite("notional", &quantModeling::FXOptionInput::notional);

    m.def("price_fx_option_analytic", [](const quantModeling::FXOptionInput &in)
          { return pricing_result_to_dict(quantModeling::price_fx_option_impl(in)); }, "Price a European FX option (Garman-Kohlhagen analytic).");

    // ── Commodity Forward ──────────────────────────────────────────────────────────
    py::class_<quantModeling::CommodityForwardInput>(m, "CommodityForwardInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::CommodityForwardInput::spot)
        .def_readwrite("rate", &quantModeling::CommodityForwardInput::rate)
        .def_readwrite("storage_cost", &quantModeling::CommodityForwardInput::storage_cost)
        .def_readwrite("convenience_yield", &quantModeling::CommodityForwardInput::convenience_yield)
        .def_readwrite("vol", &quantModeling::CommodityForwardInput::vol)
        .def_readwrite("strike", &quantModeling::CommodityForwardInput::strike)
        .def_readwrite("maturity", &quantModeling::CommodityForwardInput::maturity)
        .def_readwrite("notional", &quantModeling::CommodityForwardInput::notional);

    m.def("price_commodity_forward_analytic", [](const quantModeling::CommodityForwardInput &in)
          { return pricing_result_to_dict(quantModeling::price_commodity_forward_impl(in)); }, "Price a commodity forward (analytic).");

    // ── Commodity Option ───────────────────────────────────────────────────────────
    py::class_<quantModeling::CommodityOptionInput>(m, "CommodityOptionInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::CommodityOptionInput::spot)
        .def_readwrite("rate", &quantModeling::CommodityOptionInput::rate)
        .def_readwrite("storage_cost", &quantModeling::CommodityOptionInput::storage_cost)
        .def_readwrite("convenience_yield", &quantModeling::CommodityOptionInput::convenience_yield)
        .def_readwrite("vol", &quantModeling::CommodityOptionInput::vol)
        .def_readwrite("strike", &quantModeling::CommodityOptionInput::strike)
        .def_readwrite("maturity", &quantModeling::CommodityOptionInput::maturity)
        .def_readwrite("is_call", &quantModeling::CommodityOptionInput::is_call)
        .def_readwrite("notional", &quantModeling::CommodityOptionInput::notional);

    m.def("price_commodity_option_analytic", [](const quantModeling::CommodityOptionInput &in)
          { return pricing_result_to_dict(quantModeling::price_commodity_option_impl(in)); }, "Price a European commodity option (Black 76 analytic).");

    // ── Worst-of Option ────────────────────────────────────────────────────────────
    py::class_<quantModeling::RainbowBSInput>(m, "RainbowBSInput")
        .def(py::init<>())
        .def_readwrite("spots", &quantModeling::RainbowBSInput::spots)
        .def_readwrite("vols", &quantModeling::RainbowBSInput::vols)
        .def_readwrite("dividends", &quantModeling::RainbowBSInput::dividends)
        .def_readwrite("correlations", &quantModeling::RainbowBSInput::correlations)
        .def_readwrite("maturity", &quantModeling::RainbowBSInput::maturity)
        .def_readwrite("strike", &quantModeling::RainbowBSInput::strike)
        .def_readwrite("is_call", &quantModeling::RainbowBSInput::is_call)
        .def_readwrite("rate", &quantModeling::RainbowBSInput::rate)
        .def_readwrite("notional", &quantModeling::RainbowBSInput::notional)
        .def_readwrite("n_paths", &quantModeling::RainbowBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::RainbowBSInput::seed);

    m.def("price_worst_of_bs_mc", [](const quantModeling::RainbowBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_worst_of_impl(in)); }, "Price a worst-of option under multi-asset BS (Monte Carlo).");

    m.def("price_best_of_bs_mc", [](const quantModeling::RainbowBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_best_of_impl(in)); }, "Price a best-of option under multi-asset BS (Monte Carlo).");

    // ── SABR calibration ──────────────────────────────────────────────────────────────
    py::class_<quantModeling::SABRSliceQuote>(m, "SABRSliceQuote")
        .def(py::init<>())
        .def_readwrite("strike", &quantModeling::SABRSliceQuote::strike)
        .def_readwrite("market_iv", &quantModeling::SABRSliceQuote::market_iv)
        .def_readwrite("weight", &quantModeling::SABRSliceQuote::weight);

    m.def("calibrate_sabr_slice", &calibrate_sabr_slice_impl,
          py::arg("quotes"), py::arg("forward"), py::arg("ttm"), py::arg("beta") = 0.5,
          "Fit SABR (alpha, rho, nu; beta fixed) to one maturity slice's quotes via "
          "Levenberg-Marquardt multi-start.");

    // ── Path simulation (display/illustration, not a pricing engine) ────────────────
    m.def("simulate_black_scholes_paths", &simulate_black_scholes_paths_impl,
          py::arg("spot"), py::arg("rate"), py::arg("dividend"), py::arg("vol"), py::arg("ttm"),
          py::arg("n_steps") = 100, py::arg("n_paths") = 30, py::arg("seed") = 1,
          "Simulate exact GBM paths under Black-Scholes -- for a display chart, not pricing "
          "(use price_vanilla_* for that).");

    m.def("simulate_sabr_paths", &simulate_sabr_paths_impl,
          py::arg("forward"), py::arg("alpha"), py::arg("beta"), py::arg("rho"), py::arg("nu"),
          py::arg("ttm"), py::arg("n_steps") = 100, py::arg("n_paths") = 30, py::arg("seed") = 1,
          "Simulate illustrative SABR paths via Euler discretisation -- for a display chart, "
          "not pricing (see market/sabr_pde.hpp for arbitrage-free pricing).");
}
