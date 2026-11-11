// Python bindings, scripting (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/engines/mc/path_simulation.hpp"
#include "quantModeling/market/heston_calibration.hpp"
#include "quantModeling/market/slv_calibration.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/equity/simulatable_asian.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/calendars.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/market/conventions.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/market/vol_surface_pipeline.hpp"
#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/utils/thread_pool.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/scripting/model_advice.hpp"
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

static py::dict recommendation_to_py(const quantModeling::scripting::Recommendation &r)
{
    py::dict d;
    d["model"] = std::string(quantModeling::scripting::model_name(r.model));
    d["code"] = r.code;
    d["reason"] = r.reason;
    return d;
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
// a separate choice, made here (recommend_script_model says which one the
// script needs; the API picks it when asked for 'auto'):
//   model = "black_scholes": one flat vol (`vol`); for a script reading
//                            spot(1), spot(2)...: `spots`, `dividends`,
//                            `vols` (one per asset) and `correlation`
//                            (row-major n x n).
//   model = "local_vol":     a Dupire surface (K_grid / T_grid /
//                            sigma_loc_flat, the shape calibrate_vol_surface
//                            returns), simulated by Euler steps of
//                            1/steps_per_year.
//   model = "heston":        calibrated Heston parameters (`heston`, the
//                            dict calibrate_heston returns).
//   model = "slv":           `heston` plus the leverage grid
//                            calibrate_slv_leverage returns (K_grid /
//                            T_grid / leverage_flat).
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
                             int steps_per_year,
                             const std::map<std::string, std::vector<double>>
                                 &historical_fixings,
                             const std::map<std::string, double> &heston,
                             const std::vector<double> &leverage_flat,
                             const std::vector<double> &spots,
                             const std::vector<double> &dividends,
                             const std::vector<double> &vols,
                             const std::vector<double> &correlation,
                             const std::string &device, const std::string &rng,
                             bool control_variate, bool antithetic, bool importance_sampling,
                             const std::vector<std::vector<double>> &K_grids,
                             const std::vector<std::vector<double>> &T_grids,
                             const std::vector<std::vector<double>> &sigma_loc_flats,
                             const std::map<std::string, double> &rough_bergomi,
                             const std::vector<double> &xi_times,
                             const std::vector<double> &xi_values,
                             const std::map<std::string, double> &hull_white,
                             const std::vector<double> &curve_times,
                             const std::vector<double> &curve_dfs, std::size_t threads)
{
    using namespace quantModeling;

    const Date valuation = Date::from_iso(valuation_date);
    // Past events (on or before the valuation date) are replayed once against
    // these observed spots (ScriptedProduct, lot 16e): a seasoned product's
    // running average, extremum or barrier state enters today's price.
    std::map<Date, std::vector<double>> fixings;
    for (const auto &[iso, spots] : historical_fixings)
        fixings.emplace(Date::from_iso(iso), spots);
    const DayCounter &basis = day_counter_by_name(day_count);
    const ValuationContext ctx{valuation, &basis, &NullCalendar::instance()};
    const int paths = n_paths > 0 ? n_paths : 200000;
    const std::uint64_t seed_value = static_cast<std::uint64_t>(seed > 0 ? seed : 1);
    if (steps_per_year < 1)
        throw std::invalid_argument("price_script: steps_per_year must be >= 1");
    const double max_dt = 1.0 / static_cast<double>(steps_per_year);

    scripting::ScriptModelSpec spec;
    spec.model = model;
    spec.spot = spot;
    spec.rate = rate;
    spec.dividend = dividend;
    spec.vol = vol;
    spec.K_grid = K_grid;
    spec.T_grid = T_grid;
    spec.sigma_loc_flat = sigma_loc_flat;
    spec.leverage_flat = leverage_flat;
    spec.spots = spots;
    spec.dividends = dividends;
    spec.vols = vols;
    spec.correlation = correlation;
    spec.K_grids = K_grids;
    spec.T_grids = T_grids;
    spec.sigma_loc_flats = sigma_loc_flats;
    spec.max_dt = max_dt;
    if (model == "rough_bergomi")
    {
        const auto get = [&](const char *k, double fallback)
        {
            const auto it = rough_bergomi.find(k);
            return it == rough_bergomi.end() ? fallback : it->second;
        };
        spec.rb_H = get("H", spec.rb_H);
        spec.rb_eta = get("eta", spec.rb_eta);
        spec.rb_rho = get("rho", spec.rb_rho);
        spec.xi_times = xi_times;
        spec.xi_values = xi_values;
    }
    if (model == "hull_white")
    {
        const auto get = [&](const char *k, double fallback)
        {
            const auto it = hull_white.find(k);
            return it == hull_white.end() ? fallback : it->second;
        };
        spec.hw_mean_reversion = get("mean_reversion", spec.hw_mean_reversion);
        spec.hw_sigma = get("sigma", spec.hw_sigma);
        spec.hw_rho = get("rho", spec.hw_rho);
        spec.curve_times = curve_times;
        spec.curve_dfs = curve_dfs;
    }

    scripting::ModelKind kind = scripting::ModelKind::BlackScholesFlatVol;
    std::string model_note =
        spots.size() > 1
            ? " | model black_scholes (" + std::to_string(spots.size()) +
                  " correlated assets, flat vols)"
            : " | model black_scholes (flat vol)";
    const bool multi_local_vol = spots.size() > 1 && model == "local_vol";
    if (spots.size() > 1 && model != "black_scholes" && !multi_local_vol)
        throw std::invalid_argument(
            "price_script: model '" + model +
            "' is single-underlying; several underlyings are priced under "
            "black_scholes or local_vol");
    const std::string grid = std::to_string(K_grid.size()) + "x" +
                             std::to_string(T_grid.size());
    const std::string steps = std::to_string(steps_per_year) + " steps/yr";
    if (multi_local_vol)
    {
        kind = scripting::ModelKind::LocalVolSurface;
        model_note = " | model local_vol (" + std::to_string(spots.size()) +
                     " correlated assets, one Dupire surface each, " + steps + ")";
    }
    else if (model == "local_vol")
    {
        kind = scripting::ModelKind::LocalVolSurface;
        model_note = " | model local_vol (" + grid + " surface, " + steps + ")";
    }
    else if (model == "rough_bergomi")
    {
        kind = scripting::ModelKind::Heston; // a stochastic vol: its own forward smile
        model_note = " | model rough_bergomi (H=" + std::to_string(spec.rb_H) + " eta=" +
                     std::to_string(spec.rb_eta) + " rho=" + std::to_string(spec.rb_rho) +
                     ", forward variance curve, hybrid scheme on a daily grid)";
    }
    else if (model == "hull_white")
    {
        model_note = " | model hull_white (flat equity vol, Hull-White rates a=" +
                     std::to_string(spec.hw_mean_reversion) + " sigma=" + std::to_string(spec.hw_sigma) +
                     " rho=" + std::to_string(spec.hw_rho) +
                     (curve_times.empty() ? ", flat curve)" : ", fitted to the discount curve)");
    }
    else if (model == "heston" || model == "slv")
    {
        spec.heston = heston_from_py(heston);
        kind = model == "heston" ? scripting::ModelKind::Heston
                                 : scripting::ModelKind::StochasticLocalVol;
        model_note = model == "heston"
                         ? " | model heston (" + steps + ")"
                         : " | model slv (" + grid + " leverage, " + steps + ")";
    }
    const bool has_surface = kind == scripting::ModelKind::LocalVolSurface ||
                             kind == scripting::ModelKind::StochasticLocalVol;
    double surface_T = (has_surface && !T_grid.empty()) ? T_grid.back() : 0.0;
    if (multi_local_vol) // the shortest of the surfaces
        for (std::size_t i = 0; i < T_grids.size(); ++i)
            if (!T_grids[i].empty())
                surface_T = i == 0 ? T_grids[i].back() : std::min(surface_T, T_grids[i].back());

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
        // The tape is thread_local (aad::Number::tape), so the adjoint
        // simulation releases the GIL like the plain one: independent AAD
        // runs (the superbucket's batches) go in parallel on Python threads.
        //
        // On the GPU (blueprint/wp/19-gpu.md §6, lot G3) there is no tape:
        // duals under Black-Scholes and Heston, the per-path adjoint under
        // local vol -- same labels, same Philox paths as the CPU tape with
        // rng = philox. 'auto' falls back to the tape and says why.
        std::optional<AADSimulResults> aad_res;
        std::vector<scripting::Advice> aad_advice;
        std::size_t aad_events = 0;
        std::string aad_device = "cpu", gpu_note, lsm_note;
        // Both adjoint engines take Sobol RQMC with the Brownian bridge
        // (blueprint/wp/19-gpu.md §2.2); stratification stays pseudo-random.
        const SamplerKind aad_sampler = sampler == "sobol" ? SamplerKind::Sobol : SamplerKind::PseudoRandom;
        {
            py::gil_scoped_release release;
            // Early exercise: the rule is fitted once, on doubles, and the
            // adjoint pass differentiates the price under it (the decision is
            // held fixed: at the optimal boundary its own sensitivity
            // vanishes, the envelope argument).
            std::optional<scripting::ExercisePolicy> policy;
            {
                ScriptedProduct<Real> probe(script, ctx, ScriptSettings{fuzzy, default_eps}, fixings);
                if (probe.has_exercise())
                {
                    auto m = scripting::make_script_model<Real>(spec);
                    check_underlyings(probe.n_underlyings(), m->n_underlyings());
                    lsm_note = " | " + fit_exercise_policy(probe, *m, paths, seed_value).note();
                    policy = probe.exercise_policy();
                    if (device == "gpu")
                        throw InvalidInput("GPU: early exercise (exercise() / call()) runs on the CPU only");
                    gpu_note = " (on the CPU: early exercise)";
                }
            }
            if (!policy && (device == "gpu" || device == "auto"))
            {
                ScriptedProduct<Real> product(script, ctx, ScriptSettings{fuzzy, default_eps}, fixings);
                auto sim_model = scripting::make_script_model<Real>(spec);
                check_underlyings(product.n_underlyings(), sim_model->n_underlyings());
                std::string why;
                aad_res = simulate_script_aad_gpu(product, *sim_model, static_cast<std::size_t>(paths),
                                                  seed_value, why, aad_sampler);
                if (aad_res)
                {
                    aad_device = "gpu";
                    aad_events = product.timeline().size();
                    aad_advice = scripting::advise(product.analysis(), kind,
                                                   product.timeline().back(), surface_T);
                }
                else if (device == "gpu")
                    throw InvalidInput("GPU: " + why);
                else
                    gpu_note = " (on the CPU: " + why + ")";
            }
            if (!aad_res)
            {
                ScriptedProduct<aad::Number> product(
                    script, ctx, ScriptSettings{fuzzy, default_eps}, fixings);
                if (policy)
                    product.set_exercise_policy(*policy);
                auto sim_model = scripting::make_script_model<aad::Number>(spec);
                check_underlyings(product.n_underlyings(), sim_model->n_underlyings());
                aad_res = simulate_aad(product, *sim_model, static_cast<std::size_t>(paths),
                                       seed_value, first_aad_payoff,
                                       rng == "philox" ? RngKind::Philox : RngKind::Pcg32, aad_sampler);
                aad_events = product.timeline().size();
                aad_advice = scripting::advise(product.analysis(), kind,
                                               product.timeline().back(), surface_T);
            }
        }

        PricingResult res = to_pricing_result(*aad_res);
        res.diagnostics += gpu_note + " | scripted, " + std::to_string(aad_events) + " events" +
                           (fuzzy ? ", fuzzy" : ", hard") + model_note + lsm_note;
        res.device = aad_device;
        if (sampler == "stratified")
            res.diagnostics += " | stratified sampling is not available under AAD -- used "
                               "pseudo-random instead";
        if (importance_sampling)
            res.diagnostics += " | importance sampling is not available under AAD";
        py::dict out = pricing_result_to_dict(res);
        out["warnings"] = advice_to_py(aad_advice);
        return out;
    }

    // The simulation is pure C++ on arguments pybind11 has already converted:
    // release the GIL for it, so that several scripts (a portfolio's
    // positions, a history's days) can be priced on parallel Python threads.
    // Only building the Python result needs it back.
    std::optional<SimulationMCResult> mc;
    std::vector<scripting::Advice> advice;
    std::size_t n_events = 0;
    std::string lsm_note;
    {
        py::gil_scoped_release release;
        ScriptedProduct<Real> product(script, ctx, ScriptSettings{fuzzy, default_eps},
                                      fixings);
        auto sim_model = scripting::make_script_model<Real>(spec);
        check_underlyings(product.n_underlyings(), sim_model->n_underlyings());

        PricingSettings settings;
        settings.mc_paths = paths;
        settings.mc_seed = static_cast<int>(seed_value);
        settings.mc_antithetic = antithetic;
        settings.mc_sampler = sampler == "sobol"        ? SamplerKind::Sobol
                              : sampler == "stratified" ? SamplerKind::Stratified
                                                        : SamplerKind::PseudoRandom;
        // blueprint/wp/19-gpu.md §2.5: the deflated spots as control variates.
        settings.mc_spot_control = control_variate;
        // ... and a GHS drift, kept when a pilot shows it pays.
        settings.mc_importance_drift = importance_sampling;
        // blueprint/wp/19-gpu.md §8: CPU, GPU, or GPU when present.
        settings.mc_device = device == "gpu"    ? ComputeDevice::Gpu
                             : device == "auto" ? ComputeDevice::Auto
                                                : ComputeDevice::Cpu;
        settings.mc_rng = (rng == "philox") ? RngKind::Philox : RngKind::Pcg32;

        if (product.has_exercise())
            lsm_note = " | " + fit_exercise_policy(product, *sim_model, paths, seed_value).note();
        // blueprint/wp/26-performance.md, P1e: the CPU run on several
        // threads, the same bits on any number of them (the default, as the
        // xVA's: the available CPUs, at most 16).
        ThreadPool pool;
        const std::size_t workers =
            threads > 0 ? threads - 1 : std::min<std::size_t>(available_cpus(), 16) - 1;
        if (workers > 0)
            pool.start(workers);
        mc = simulate_script(product, *sim_model, settings, workers > 0 ? &pool : nullptr);
        n_events = product.timeline().size();
        advice = scripting::advise(product.analysis(), kind, product.timeline().back(),
                                   surface_T);
    }

    PricingResult res;
    res.npv = mc->npv();
    res.mc_std_error = mc->std_error();
    res.diagnostics = mc->diagnostics + " | scripted, " + std::to_string(n_events) +
                      " events" + (fuzzy ? ", fuzzy" : ", hard") + model_note + lsm_note;
    res.device = mc->device;
    res.gpus = mc->gpus;
    py::dict out = pricing_result_to_dict(res);
    out["warnings"] = advice_to_py(advice);
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
    // What 'auto' picks when the market surface and its stochastic-vol
    // calibration are both available -- the usual case, announced before
    // pricing; price_script's caller reports the actual choice.
    out["recommendation"] = recommendation_to_py(
        scripting::recommend(a, scripting::ModelAvailability{true, true}));
    return out;
}

void register_scripting(py::module_ &m)
{
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
          py::arg("historical_fixings") =
              std::map<std::string, std::vector<double>>{},
          py::arg("heston") = std::map<std::string, double>{},
          py::arg("leverage_flat") = std::vector<double>{},
          py::arg("spots") = std::vector<double>{},
          py::arg("dividends") = std::vector<double>{},
          py::arg("vols") = std::vector<double>{},
          py::arg("correlation") = std::vector<double>{},
          py::arg("device") = "cpu", py::arg("rng") = "pcg32",
          py::arg("control_variate") = false, py::arg("antithetic") = true,
          py::arg("importance_sampling") = false,
          py::arg("K_grids") = std::vector<std::vector<double>>{},
          py::arg("T_grids") = std::vector<std::vector<double>>{},
          py::arg("sigma_loc_flats") = std::vector<std::vector<double>>{},
          py::arg("rough_bergomi") = std::map<std::string, double>{},
          py::arg("xi_times") = std::vector<double>{},
          py::arg("xi_values") = std::vector<double>{},
          py::arg("hull_white") = std::map<std::string, double>{},
          py::arg("curve_times") = std::vector<double>{},
          py::arg("curve_dfs") = std::vector<double>{}, py::arg("threads") = 0,
          "Price a payoff script (blueprint/wp/16-scripting.md) via the "
          "timeline simulation engine. threads: CPU threads of a CPU run "
          "(0, the default: the available CPUs, at most 16); Philox, Sobol and "
          "stratified runs give the same bits on any number of them, a PCG32 "
          "run and a script with early exercise stay on one thread "
          "(blueprint/wp/26-performance.md, P1e). Raises on a malformed script, with "
          "the offending line and column in the message. greeks_method: "
          "'none' (default) or 'aad' -- every model parameter's sensitivity "
          "(blueprint/wp/17-aad.md), at roughly 3-5x one price's cost "
          "regardless of how many; on the GPU (device 'gpu' or 'auto') by "
          "forward-mode duals or the per-path adjoint (blueprint/wp/19-gpu.md "
          "§6). 'bump' is not offered for scripted payoffs. sampler: "
          "'pseudo', 'sobol' or 'stratified' (terminal value stratified, "
          "path by Brownian bridge); control_variate: regress the deflated "
          "spots out of the payoff (blueprint/wp/19-gpu.md §2.5); antithetic: "
          "pair each path with its mirror (price only; the default); "
          "importance_sampling: a Glasserman-Heidelberger-Shahabuddin drift, "
          "kept only when a pilot shows it lowers the variance (price only). "
          "Sobol runs on the GPU and under AAD too. model 'hull_white': the "
          "equity under stochastic Hull-White rates, hull_white = {mean_reversion, "
          "sigma, rho}, fitted to the discount curve (curve_times, curve_dfs) or "
          "to the flat `rate` when they are empty; df() reads the simulated curve.");
    m.def(
        "recommend_script_model",
        [](const std::string &script, const std::string &valuation_date,
           const std::string &day_count, bool market_surface, bool stochastic)
        {
            using namespace quantModeling;
            const Date valuation = Date::from_iso(valuation_date);
            const ValuationContext ctx{valuation, &day_counter_by_name(day_count),
                                       &NullCalendar::instance()};
            const ScriptedProduct<Real> product(script, ctx);
            return recommendation_to_py(scripting::recommend(
                product.analysis(),
                scripting::ModelAvailability{market_surface, stochastic}));
        },
        py::arg("script"), py::arg("valuation_date"), py::arg("day_count") = "ACT/365F",
        py::arg("market_surface") = true, py::arg("stochastic") = true,
        "The simplest model that captures what the script's price depends on "
        "(scripting/model_advice.hpp recommend()), among those available: "
        "{model, code, reason}.");
    m.def("validate_script", &validate_script, py::arg("script"),
          py::arg("valuation_date"), py::arg("day_count") = "ACT/365F",
          "Parse a payoff script and resolve its timeline without pricing "
          "it. Raises the same way price_script does on a malformed script.");
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
