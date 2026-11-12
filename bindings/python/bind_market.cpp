// Python bindings, market (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla_lv.hpp"
#include "quantModeling/engines/mc/path_simulation.hpp"
#include "quantModeling/market/heston_calibration.hpp"
#include "quantModeling/market/sabr_calibration.hpp"
#include "quantModeling/market/slv_calibration.hpp"
#include "quantModeling/market/superbucket.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/vol_surface_pipeline.hpp"
#include "quantModeling/market/rough_bergomi_calibration.hpp"
#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/utils/thread_pool.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
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

// ── Rough Bergomi (market/rough_bergomi_calibration.hpp) ─────────────────────

static py::dict calibrate_rough_bergomi_impl(const std::vector<std::tuple<double, double, double>> &targets,
                                             const std::vector<double> &ttm,
                                             const std::vector<double> &total_variance, int n_paths,
                                             int steps_per_year, std::uint64_t seed)
{
    using namespace quantModeling;
    std::vector<RoughBergomiTarget> t;
    for (const auto &[T, k, v] : targets)
        t.push_back({T, k, v});
    RoughBergomiSurfaceSettings mc;
    mc.n_paths = n_paths;
    mc.steps_per_year = steps_per_year;
    mc.seed = seed;
    RoughBergomiCalibration c;
    {
        py::gil_scoped_release release;
        c = calibrate_rough_bergomi(t, ttm, total_variance, mc);
    }
    py::dict out;
    out["H"] = c.params.H;
    out["eta"] = c.params.eta;
    out["rho"] = c.params.rho;
    out["xi_times"] = c.xi.times;
    out["xi_values"] = c.xi.xi;
    out["xi_floored"] = c.xi_floored;
    out["iv_rmse"] = c.iv_rmse;
    out["iv_worst"] = c.iv_worst;
    out["n_quotes"] = c.n_quotes;
    out["n_unpriced"] = c.n_unpriced;
    out["iterations"] = c.report.iterations;
    out["converged"] = c.report.converged;
    out["seconds"] = c.report.wall_time_seconds;
    out["model_vols"] = c.model_vols;
    out["model_vol_errors"] = c.model_vol_errors;
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
        sd["in_surface"] = s.in_surface;
        py::list quotes;
        for (const auto &q : s.quotes)
            quotes.append(py::make_tuple(q.log_moneyness, q.market_iv, q.weight));
        sd["quotes"] = quotes;
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

// ── Stochastic-vol calibration: Heston on an implied-vol surface, then the
//    SLV leverage on the Dupire grid of that same surface ───────────────────

static py::dict calibrate_heston_impl(const std::vector<double> &strikes,
                                      const std::vector<double> &ttms,
                                      const std::vector<double> &implied_vols,
                                      double spot, double rate, double dividend)
{
    using namespace quantModeling;
    if (strikes.size() != ttms.size() || strikes.size() != implied_vols.size())
        throw std::invalid_argument("calibrate_heston: strikes, ttms and implied_vols must have the same length");

    std::vector<HestonCalibrationQuote> quotes;
    quotes.reserve(strikes.size());
    for (std::size_t i = 0; i < strikes.size(); ++i)
        quotes.push_back({strikes[i], ttms[i], implied_vols[i], 1.0});

    HestonCalibration res;
    {
        py::gil_scoped_release release;
        res = calibrate_heston(std::move(quotes), spot, rate, dividend);
    }

    py::dict out;
    out["v0"] = res.params.v0;
    out["kappa"] = res.params.kappa;
    out["theta"] = res.params.theta;
    out["xi"] = res.params.xi;
    out["rho"] = res.params.rho;
    out["iv_rmse"] = res.iv_rmse;
    out["iv_worst"] = res.iv_worst;
    out["n_quotes"] = res.n_quotes;
    out["n_unpriced"] = res.n_unpriced;
    out["n_maturities"] = res.n_maturities;
    out["n_starts"] = res.n_starts;
    out["iterations"] = res.report.iterations;
    out["converged"] = res.report.converged;
    out["feller"] = res.feller;
    out["wall_time_seconds"] = res.report.wall_time_seconds;
    out["iv_errors"] = res.iv_errors; // per quote, input order; NaN: unpriced
    return out;
}

static py::dict calibrate_slv_leverage_impl(double spot, double rate, double dividend,
                                            const std::map<std::string, double> &heston,
                                            const std::vector<double> &K_grid,
                                            const std::vector<double> &T_grid,
                                            const std::vector<double> &sigma_loc_flat,
                                            std::size_t n_particles, int seed)
{
    using namespace quantModeling;
    const HestonParams h = heston_from_py(heston);
    SLVCalibrationSettings settings;
    settings.n_particles = n_particles;
    settings.seed = static_cast<std::uint64_t>(seed > 0 ? seed : 1);

    SLVLeverageGrid grid;
    {
        py::gil_scoped_release release;
        grid = calibrate_slv_leverage(spot, rate, dividend, h, K_grid, T_grid,
                                      sigma_loc_flat, settings);
    }
    py::dict out;
    out["K_grid"] = grid.K_grid;
    out["T_grid"] = grid.T_grid;
    out["leverage_flat"] = grid.leverage;
    out["n_particles"] = settings.n_particles;
    out["clamped_mass"] = grid.clamped_mass;
    // The clamp a bucket's estimate is held to where E[v|S] is unreliable:
    // a caller can count how much of the grid sits on it.
    out["leverage_floor"] = settings.leverage_floor;
    out["leverage_cap"] = settings.leverage_cap;
    return out;
}

// ── Dupire superbucket (lot 17h): dV/dsigma_loc -> dV/d(quoted implied vol) ─

static py::dict dupire_superbucket_impl(const py::list &slices_py, double spot, double rate,
                                        double dividend, double k_min, double k_max,
                                        std::size_t n_strikes, std::size_t n_maturities,
                                        const std::vector<double> &dV_dsigma_loc)
{
    using namespace quantModeling;
    std::vector<SVISliceCalibration> slices;
    std::vector<std::vector<SVISliceQuote>> quotes;
    for (const py::handle h : slices_py)
    {
        const py::dict d = py::reinterpret_borrow<py::dict>(h);
        // Only the slices the grid was built from (calibrate_vol_surface's
        // select_surface_slices); a slice left out moves no local vol.
        if (d.contains("in_surface") && !d["in_surface"].cast<bool>())
            continue;
        SVISliceCalibration c;
        c.ttm = d["ttm"].cast<double>();
        c.params = SVIParams{d["a"].cast<double>(), d["b"].cast<double>(),
                             d["rho"].cast<double>(), d["m"].cast<double>(),
                             d["sigma"].cast<double>()};
        std::vector<SVISliceQuote> q;
        for (const py::handle t : d["quotes"].cast<py::list>())
        {
            const auto [k, iv, w] = t.cast<std::tuple<double, double, double>>();
            q.push_back({k, iv, w});
        }
        slices.push_back(c);
        quotes.push_back(std::move(q));
    }
    SuperbucketResult r;
    {
        py::gil_scoped_release release;
        r = dupire_superbucket(slices, quotes, spot, rate, dividend, k_min, k_max, n_strikes,
                               n_maturities, dV_dsigma_loc);
    }
    py::list rows;
    for (const QuoteVega &v : r.quotes)
    {
        py::dict d;
        d["slice"] = v.slice;
        d["ttm"] = v.ttm;
        d["log_moneyness"] = v.log_moneyness;
        d["strike"] = v.strike;
        d["implied_vol"] = v.implied_vol;
        d["vega"] = v.vega;
        rows.append(d);
    }
    py::dict out;
    out["quotes"] = rows;
    out["dV_dsvi"] = r.dV_dsvi;
    out["at_bound"] = r.at_bound;
    return out;
}

void register_market(py::module_ &m)
{
    m.def("dupire_superbucket", &dupire_superbucket_impl, py::arg("slices"), py::arg("spot"),
          py::arg("rate"), py::arg("dividend"), py::arg("k_min"), py::arg("k_max"),
          py::arg("n_strikes"), py::arg("n_maturities"), py::arg("dV_dsigma_loc"),
          "dV / d(each quoted implied vol) from dV/dsigma_loc (the local-vol AAD "
          "risks, K-major): through Dupire on an AAD tape, then through each SVI "
          "fit by the implicit function theorem (market/superbucket.hpp, lot 17h). "
          "`slices` are calibrate_vol_surface's, with their `quotes`.");
    m.def("calibrate_heston", &calibrate_heston_impl, py::arg("strikes"),
          py::arg("ttms"), py::arg("implied_vols"), py::arg("spot"), py::arg("rate"),
          py::arg("dividend"),
          "Fit Heston (v0, kappa, theta, xi, rho) to an implied-vol surface by "
          "Levenberg-Marquardt on COS prices (market/heston_calibration.hpp). "
          "iv_rmse / iv_worst are the exact implied-vol errors, in vol points.");
    m.def("calibrate_slv_leverage", &calibrate_slv_leverage_impl, py::arg("spot"),
          py::arg("rate"), py::arg("dividend"), py::arg("heston"), py::arg("K_grid"),
          py::arg("T_grid"), py::arg("sigma_loc_flat"), py::arg("n_particles") = 50000,
          py::arg("seed") = 1,
          "Leverage L(K, T) such that Heston dynamics times L reproduce the "
          "given Dupire grid's marginals (particle method, "
          "market/slv_calibration.hpp). Same K-major layout as sigma_loc_flat.");
    m.def("calibrate_rough_bergomi", &calibrate_rough_bergomi_impl, py::arg("targets"), py::arg("ttm"),
          py::arg("total_variance"), py::arg("n_paths") = 20000, py::arg("steps_per_year") = 400,
          py::arg("seed") = 1,
          "Calibrate rough Bergomi (H, eta, rho) to implied vols [(ttm, k = ln K/F, vol)] on the "
          "forward variance curve of the variance-swap total variances w_VS(T) at `ttm`; Monte-Carlo on "
          "common random numbers.");
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
    m.def("calibrate_sabr_slice", &calibrate_sabr_slice_impl,
          py::arg("quotes"), py::arg("forward"), py::arg("ttm"), py::arg("beta") = 0.5,
          "Fit SABR (alpha, rho, nu; beta fixed) to one maturity slice's quotes via "
          "Levenberg-Marquardt multi-start.");

    // ── Path simulation (display/illustration, not a pricing engine) ────────────────
    m.def(
        "simulate_model_paths",
        [](const std::string &model, double spot, double rate, double dividend, double vol,
           const std::vector<double> &K_grid, const std::vector<double> &T_grid,
           const std::vector<double> &sigma_loc_flat,
           const std::map<std::string, double> &heston,
           const std::vector<double> &leverage_flat, double ttm, std::size_t n_steps,
           long long n_paths, int seed, int steps_per_year)
        {
            using namespace quantModeling;
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
            if (model == "heston" || model == "slv")
                spec.heston = heston_from_py(heston);
            spec.max_dt = 1.0 / static_cast<double>(std::max(steps_per_year, 1));

            PathSimulationSettings settings;
            settings.n_steps = n_steps;
            settings.n_paths = n_paths;
            settings.seed = static_cast<std::uint64_t>(seed > 0 ? seed : 1);
            SimulatedPaths res;
            {
                py::gil_scoped_release release;
                auto sim = scripting::make_script_model<Real>(spec);
                res = simulate_model_paths(*sim, spot, ttm, settings);
            }
            py::dict out;
            out["time_grid"] = res.time_grid;
            out["paths"] = res.paths;
            return out;
        },
        py::arg("model"), py::arg("spot"), py::arg("rate"), py::arg("dividend"),
        py::arg("vol") = 0.0, py::arg("K_grid") = std::vector<double>{},
        py::arg("T_grid") = std::vector<double>{},
        py::arg("sigma_loc_flat") = std::vector<double>{},
        py::arg("heston") = std::map<std::string, double>{},
        py::arg("leverage_flat") = std::vector<double>{}, py::arg("ttm") = 1.0,
        py::arg("n_steps") = 100, py::arg("n_paths") = 30, py::arg("seed") = 1,
        py::arg("steps_per_year") = 252,
        "Illustrative paths of a script model (black_scholes, local_vol, heston, slv) "
        "with its calibrated inputs, on a regular grid of n_steps dates to ttm.");
}
