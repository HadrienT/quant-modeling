#ifndef QM_BINDINGS_PYTHON_COMMON_HPP
#define QM_BINDINGS_PYTHON_COMMON_HPP

// What the binding units share (bindings/python/bind_*.cpp): pybind11, the
// helpers more than one domain uses, and each domain's registration. The
// module was one translation unit, src/module.cpp, which every change to one
// of its 147 headers recompiled in about 50 s; split by domain, a header change
// recompiles the units that use it, in parallel.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/equity/heston.hpp"
#include "quantModeling/pricers/registry.hpp"

namespace py = pybind11;
namespace qm_ = quantModeling;

inline py::dict pricing_result_to_dict(const quantModeling::PricingResult &res)
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
    out["device"] = res.device;
    out["gpus"] = res.gpus;
    out["discretisation_bias"] = to_py(res.discretisation_bias);

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

inline quantModeling::HestonParams heston_from_py(const std::map<std::string, double> &d)
{
    auto get = [&](const char *key)
    {
        const auto it = d.find(key);
        if (it == d.end())
            throw std::invalid_argument(std::string("heston parameters need '") + key + "'");
        return it->second;
    };
    return quantModeling::HestonParams{get("v0"), get("kappa"), get("theta"), get("xi"),
                                       get("rho")};
}

inline qm_::DiscountCurve rate_curve(std::vector<qm_::Time> times, std::vector<qm_::Real> dfs)
{
    return qm_::DiscountCurve(std::move(times), std::move(dfs), qm_::CurveExtrapolation::FlatForward);
}

// Each domain's classes and functions, registered in this order by
// PYBIND11_MODULE (module.cpp): the classes and enums first, so that every
// docstring names them by their Python names.
void register_core(py::module_ &m);
void register_pricing(py::module_ &m);
void register_rates(py::module_ &m);
void register_market(py::module_ &m);
void register_xva(py::module_ &m);
void register_scripting(py::module_ &m);

#endif // QM_BINDINGS_PYTHON_COMMON_HPP
