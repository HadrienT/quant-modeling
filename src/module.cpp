#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/analytic/asian.hpp"
#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace quantModeling
{

    struct VanillaBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
    };

    struct AsianBSInput
    {
        Real spot;
        Real strike;
        Time maturity;
        Real rate;
        Real dividend;
        Real vol;
        bool is_call;
        AsianAverageType average_type = AsianAverageType::Arithmetic;

        int n_paths = 200000;
        int seed = 1;
        Real mc_epsilon = 0.0;
    };

    static PricingResult price_vanilla_impl(const VanillaBSInput &in, bool use_mc)
    {
        using quantModeling::OptionType;

        auto payoff = std::make_shared<quantModeling::PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put,
            static_cast<quantModeling::Real>(in.strike));

        auto exercise = std::make_shared<quantModeling::EuropeanExercise>(
            static_cast<quantModeling::Real>(in.maturity));

        quantModeling::VanillaOption opt(payoff, exercise, 1.0);

        auto model = std::make_shared<quantModeling::BlackScholesModel>(
            static_cast<quantModeling::Real>(in.spot),
            static_cast<quantModeling::Real>(in.rate),
            static_cast<quantModeling::Real>(in.dividend),
            static_cast<quantModeling::Real>(in.vol));

        quantModeling::MarketView market = {};
        quantModeling::PricingSettings settings = {
            use_mc ? in.n_paths : 0,
            use_mc ? in.seed : 0,
            in.mc_epsilon};

        quantModeling::PricingContext ctx{market, settings, model};

        if (use_mc)
        {
            quantModeling::BSEuroVanillaMCEngine engine(ctx);
            return quantModeling::price(opt, engine);
        }

        quantModeling::BSEuroVanillaAnalyticEngine engine(ctx);
        return quantModeling::price(opt, engine);
    }

    static PricingResult price_asian_impl(const AsianBSInput &in, bool use_mc)
    {
        using quantModeling::OptionType;

        std::shared_ptr<quantModeling::IPayoff> payoff;
        if (in.average_type == AsianAverageType::Arithmetic)
        {
            payoff = std::make_shared<quantModeling::ArithmeticAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<quantModeling::Real>(in.strike));
        }
        else
        {
            payoff = std::make_shared<quantModeling::GeometricAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<quantModeling::Real>(in.strike));
        }

        auto exercise = std::make_shared<quantModeling::EuropeanExercise>(
            static_cast<quantModeling::Real>(in.maturity));

        quantModeling::AsianOption opt(payoff, exercise, in.average_type, 1.0);

        auto model = std::make_shared<quantModeling::BlackScholesModel>(
            static_cast<quantModeling::Real>(in.spot),
            static_cast<quantModeling::Real>(in.rate),
            static_cast<quantModeling::Real>(in.dividend),
            static_cast<quantModeling::Real>(in.vol));

        quantModeling::MarketView market = {};
        quantModeling::PricingSettings settings = {
            use_mc ? in.n_paths : 0,
            use_mc ? in.seed : 0,
            in.mc_epsilon};

        quantModeling::PricingContext ctx{market, settings, model};

        if (use_mc)
        {
            quantModeling::BSEuroAsianMCEngine engine(ctx);
            return quantModeling::price(opt, engine);
        }

        if (in.average_type == AsianAverageType::Arithmetic)
        {
            quantModeling::BSEuroArithmeticAsianAnalyticEngine engine(ctx);
            return quantModeling::price(opt, engine);
        }

        quantModeling::BSEuroGeometricAsianAnalyticEngine engine(ctx);
        return quantModeling::price(opt, engine);
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
        .def_readwrite("mc_epsilon", &quantModeling::VanillaBSInput::mc_epsilon);

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

    m.def("price_vanilla_bs_analytic", &price_vanilla_bs_analytic,
          "Price vanilla option under Black-Scholes (analytic).");
    m.def("price_vanilla_bs_mc", &price_vanilla_bs_mc,
          "Price vanilla option under Black-Scholes (Monte Carlo).");
    m.def("price_asian_bs_analytic", &price_asian_bs_analytic,
          "Price Asian option under Black-Scholes (analytic).");
    m.def("price_asian_bs_mc", &price_asian_bs_mc,
          "Price Asian option under Black-Scholes (Monte Carlo).");
}
