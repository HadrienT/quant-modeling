// Python bindings, core (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/market/raw_vol_surface.hpp"

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/market/heston_calibration.hpp"
#include "quantModeling/market/sabr_calibration.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/market/historical_rate_dynamics.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/fx/cross_currency_swap.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"

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

#ifndef QM_BUILD_SHA
#define QM_BUILD_SHA "unknown"
#endif

void register_core(py::module_ &m)
{
    m.doc() = "quantModeling C++ bindings (pybind11)";

    m.def(
        "build_sha", []()
        { return std::string(QM_BUILD_SHA); },
        "Git short SHA the native module was built from (blueprint WP 18a "
        "section 5.1) -- read by the API's audit trail so a valuation can be "
        "traced to the exact lib_build that produced it.");

    // ── GPU backend (blueprint/wp/19-gpu.md §8) ──
    py::register_exception<quantModeling::gpu::GpuUnavailable>(m, "GpuUnavailable", PyExc_RuntimeError);

    py::enum_<quantModeling::ComputeDevice>(m, "ComputeDevice")
        .value("Cpu", quantModeling::ComputeDevice::Cpu)
        .value("Gpu", quantModeling::ComputeDevice::Gpu)
        .value("Auto", quantModeling::ComputeDevice::Auto);

    py::enum_<quantModeling::RngKind>(m, "RngKind")
        .value("Pcg32", quantModeling::RngKind::Pcg32)
        .value("Philox", quantModeling::RngKind::Philox);

    m.def("gpu_compiled", &quantModeling::gpu::compiled_with_cuda,
          "True when the native module was built with the CUDA backend.");
    m.def(
        "gpu_devices", []()
        {
            std::vector<std::string> names;
            for (int d = 0; d < quantModeling::gpu::device_count(); ++d)
                names.push_back(quantModeling::gpu::device_name(d));
            return names; },
        "Names of the usable CUDA devices (empty on a CPU-only build or host).");
    m.def("gpu_warm_up", &quantModeling::gpu::warm_up, py::arg("device") = 0,
          py::call_guard<py::gil_scoped_release>(),
          "Create the CUDA context and load the kernels, so that the first "
          "priced request is not charged for them.");

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
        .def_readwrite("pde_time_steps", &quantModeling::VanillaBSInput::pde_time_steps)
        .def_readwrite("device", &quantModeling::VanillaBSInput::device)
        .def_readwrite("rng", &quantModeling::VanillaBSInput::rng);

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
        .def_readwrite("compute_greeks", &quantModeling::BarrierBSInput::compute_greeks)
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

    m.def("bachelier_implied_vol", &quantModeling::bachelier_implied_vol, py::arg("payer"), py::arg("price"),
          py::arg("forward"), py::arg("strike"), py::arg("expiry"), py::arg("annuity"),
          "Normal (Bachelier) implied vol of a swaption price on (forward, annuity); NaN when "
          "the price is at or below intrinsic.");
    m.def("estimate_historical_volatility", &quantModeling::estimate_historical_volatility, py::arg("rates"),
          py::arg("dt"), "Absolute volatility of a rate series, sd of its changes per sqrt(year).");
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
        .def_readwrite("compute_greeks", &quantModeling::BarrierLocalVolInput::compute_greeks)
        .def_readwrite("surface", &quantModeling::BarrierLocalVolInput::surface)
        .def_readwrite("n_paths", &quantModeling::BarrierLocalVolInput::n_paths)
        .def_readwrite("seed", &quantModeling::BarrierLocalVolInput::seed);

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

    py::class_<quantModeling::FXForwardInput>(m, "FXForwardInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::FXForwardInput::spot)
        .def_readwrite("rate_domestic", &quantModeling::FXForwardInput::rate_domestic)
        .def_readwrite("rate_foreign", &quantModeling::FXForwardInput::rate_foreign)
        .def_readwrite("vol", &quantModeling::FXForwardInput::vol)
        .def_readwrite("strike", &quantModeling::FXForwardInput::strike)
        .def_readwrite("maturity", &quantModeling::FXForwardInput::maturity)
        .def_readwrite("notional", &quantModeling::FXForwardInput::notional);

    py::class_<quantModeling::QuantoBSInput>(m, "QuantoBSInput")
        .def(py::init<>())
        .def_readwrite("spot", &quantModeling::QuantoBSInput::spot)
        .def_readwrite("strike", &quantModeling::QuantoBSInput::strike)
        .def_readwrite("maturity", &quantModeling::QuantoBSInput::maturity)
        .def_readwrite("rate_domestic", &quantModeling::QuantoBSInput::rate_domestic)
        .def_readwrite("rate_foreign", &quantModeling::QuantoBSInput::rate_foreign)
        .def_readwrite("dividend", &quantModeling::QuantoBSInput::dividend)
        .def_readwrite("vol", &quantModeling::QuantoBSInput::vol)
        .def_readwrite("fx_vol", &quantModeling::QuantoBSInput::fx_vol)
        .def_readwrite("correlation", &quantModeling::QuantoBSInput::correlation)
        .def_readwrite("fx_rate", &quantModeling::QuantoBSInput::fx_rate)
        .def_readwrite("is_call", &quantModeling::QuantoBSInput::is_call)
        .def_readwrite("n_paths", &quantModeling::QuantoBSInput::n_paths)
        .def_readwrite("seed", &quantModeling::QuantoBSInput::seed)
        .def_readwrite("mc_epsilon", &quantModeling::QuantoBSInput::mc_epsilon);

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

    py::class_<quantModeling::SABRSliceQuote>(m, "SABRSliceQuote")
        .def(py::init<>())
        .def_readwrite("strike", &quantModeling::SABRSliceQuote::strike)
        .def_readwrite("market_iv", &quantModeling::SABRSliceQuote::market_iv)
        .def_readwrite("weight", &quantModeling::SABRSliceQuote::weight);
}
