#ifndef RESULTS_HPP
#define RESULTS_HPP

#include "quantModeling/core/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace quantModeling
{

    /// How a product's Greeks were obtained. `Bump` is the only method most
    /// of the catalogue supports; `AAD` is real today only for products on
    /// the timeline architecture (blueprint/wp/17-aad.md) -- currently
    /// BlackScholesSimModel and anything written in the scripting language.
    enum class GreeksMethod
    {
        None,
        Bump,
        AAD
    };

    /// Every model-parameter sensitivity from one adjoint run (blueprint
    /// §13.1), not just the five fixed slots Greeks has room for -- a local
    /// vol grid's sensitivities (lot 17e) will not fit in "vega". `labels`
    /// come straight from ISimulationModel<T>::parameter_labels(), in the
    /// same order as `values`/`std_errors`.
    struct RiskReport
    {
        std::vector<std::string> labels;
        std::vector<Real> values;
        std::vector<Real> std_errors;
    };

    struct Greeks
    {
        std::optional<Real> delta;
        std::optional<Real> gamma;
        std::optional<Real> vega;
        std::optional<Real> theta;
        std::optional<Real> rho;
        // Standard errors (Monte Carlo uncertainty) for each Greek
        std::optional<Real> delta_std_error;
        std::optional<Real> gamma_std_error;
        std::optional<Real> vega_std_error;
        std::optional<Real> theta_std_error;
        std::optional<Real> rho_std_error;
    };

    struct BondAnalytics
    {
        std::optional<Real> macaulay_duration;
        std::optional<Real> modified_duration;
        std::optional<Real> convexity;
        std::optional<Real> dv01;
    };

    struct PricingResult
    {
        Real npv = 0.0;
        Greeks greeks;
        BondAnalytics bond_analytics;
        std::string diagnostics;
        Real mc_std_error;
        // New field last, matching the project's convention for structs
        // that could be aggregate-initialized (see PricingSettings in
        // pricers/context.hpp) -- cheap insurance against a future
        // positional PricingResult{...} silently shifting every field after
        // an insertion point.
        std::optional<RiskReport> risks;
    };
} // namespace quantModeling

#endif
