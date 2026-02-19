#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"

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

PYBIND11_MODULE(quantmodeling, m)
{
    m.doc() = "quantModeling C++ bindings (pybind11)";

    py::enum_<quantModeling::AsianAverageType>(m, "AsianAverageType")
        .value("Arithmetic", quantModeling::AsianAverageType::Arithmetic)
        .value("Geometric", quantModeling::AsianAverageType::Geometric);

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
}
