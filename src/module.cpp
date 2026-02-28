#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/engines/mc/local_vol.hpp"

#include <memory>
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

PYBIND11_MODULE(quantmodeling, m)
{
    m.doc() = "quantModeling C++ bindings (pybind11)";

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
}
