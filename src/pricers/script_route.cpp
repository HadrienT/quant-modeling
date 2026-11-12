#include "quantModeling/pricers/script_route.hpp"

#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/greeks.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{

    int days_for(Time t)
    {
        return static_cast<int>(std::lround(t * 365.0));
    }

    namespace
    {
        /// Which greek an adjoint risk label feeds: the model's spot(s),
        /// volatility (a flat vol, or every point of a local-vol grid: a
        /// parallel shift) and rate. Dividends are not a greek here.
        enum class Greek
        {
            None,
            Delta,
            Vega,
            Rho
        };

        Greek greek_of(const std::string &label)
        {
            auto starts = [&](const char *p)
            { return label.rfind(p, 0) == 0; };
            if (starts("spot"))
                return Greek::Delta;
            if (starts("vol") || starts("lvol"))
                return Greek::Vega;
            if (label == "rate")
                return Greek::Rho;
            return Greek::None;
        }

        ScriptSettings script_settings(const ScriptRoute &route, const scripting::ScriptModelSpec &spec, int days)
        {
            ScriptSettings s;
            s.fuzzy = route.fuzzy;
            if (route.control_mean)
                s.control_mean = route.control_mean(spec, days);
            return s;
        }

        SimulationMCResult run(const ScriptRoute &route, const scripting::ScriptModelSpec &spec, int days,
                               const PricingSettings &settings)
        {
            const ScriptedProduct<Real> product(route.script(days), ValuationContext{route.valuation},
                                                script_settings(route, spec, days));
            auto model = scripting::make_script_model<Real>(spec);
            return simulate_script(product, *model, settings);
        }

        /// A finite difference of runs that share their Sobol points:
        /// sum_i w_i V_i / scale, and its error from the replicates.
        struct Difference
        {
            Real value = 0.0;
            Real std_error = 0.0;
        };

        Difference difference(const std::vector<const SimulationMCResult *> &runs, const std::vector<Real> &weights,
                              Real scale)
        {
            Difference d;
            for (std::size_t i = 0; i < runs.size(); ++i)
                d.value += weights[i] * runs[i]->npv();
            d.value /= scale;
            const std::size_t B = runs.front()->replicates.size();
            bool paired = B >= 2;
            for (const SimulationMCResult *r : runs)
                paired = paired && r->replicates.size() == B;
            if (paired)
            {
                WelfordAccumulator acc;
                for (std::size_t b = 0; b < B; ++b)
                {
                    Real x = 0.0;
                    for (std::size_t i = 0; i < runs.size(); ++i)
                        x += weights[i] * runs[i]->replicates[b];
                    acc.add(x / scale);
                }
                d.std_error = acc.std_error();
            }
            else
            {
                // Independent runs (no replicates): the errors add up, an
                // upper bound on what common numbers would give.
                Real var = 0.0;
                for (std::size_t i = 0; i < runs.size(); ++i)
                    var += weights[i] * weights[i] * runs[i]->std_error() * runs[i]->std_error();
                d.std_error = std::sqrt(var) / std::fabs(scale);
            }
            return d;
        }
    } // namespace

    PricingResult price_by_script(const ScriptRoute &route, const PricingSettings &settings, bool greeks)
    {
        const SimulationMCResult base = run(route, route.model, 0, settings);
        PricingResult res;
        res.npv = base.npv();
        res.mc_std_error = base.std_error();
        res.device = base.device;
        res.gpus = base.gpus;
        res.diagnostics = "WP 26 script route (" + route.name + "): " + base.diagnostics;
        if (!greeks)
        {
            res.diagnostics += " | price only";
            return res;
        }

        // Delta, vega, rho: one adjoint run on the CPU tape, without the
        // control (the adjoint engine has none: unbiased, wider).
        if (!route.discontinuous)
        {
            const ScriptedProduct<aad::Number> product(route.script(0), ValuationContext{route.valuation},
                                                       ScriptSettings{route.fuzzy});
            auto model = scripting::make_script_model<aad::Number>(route.model);
            const std::size_t paths = static_cast<std::size_t>(settings.mc_paths > 0 ? settings.mc_paths : 100000);
            const auto seed = static_cast<std::uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
            std::vector<std::vector<Real>> batches;
            const AADSimulResults aad = simulate_aad(product, *model, paths, seed, first_aad_payoff, RngKind::Philox,
                                                     settings.mc_sampler == SamplerKind::Sobol ? SamplerKind::Sobol
                                                                                               : SamplerKind::PseudoRandom,
                                                     &batches);
            WelfordAccumulator acc[4];
            for (const std::vector<Real> &batch : batches)
            {
                Real sum[4] = {0.0, 0.0, 0.0, 0.0};
                for (std::size_t j = 0; j < batch.size() && j < aad.risk_labels.size(); ++j)
                    sum[static_cast<int>(greek_of(aad.risk_labels[j]))] += batch[j];
                for (int g = 1; g < 4; ++g)
                    acc[g].add(sum[g]);
            }
            Real total[4] = {0.0, 0.0, 0.0, 0.0};
            for (std::size_t j = 0; j < aad.risks.size(); ++j)
                total[static_cast<int>(greek_of(aad.risk_labels[j]))] += aad.risks[j];
            res.greeks.delta = total[1];
            res.greeks.vega = total[2];
            res.greeks.rho = total[3];
            if (batches.size() >= 2)
            {
                res.greeks.delta_std_error = acc[1].std_error();
                res.greeks.vega_std_error = acc[2].std_error();
                res.greeks.rho_std_error = acc[3].std_error();
            }
        }

        // Gamma and theta: central differences on the CPU, Sobol points
        // shared with the base run (same seed), errors replicate-wise.
        PricingSettings cpu = settings;
        cpu.mc_device = ComputeDevice::Cpu;
        const SimulationMCResult centre = settings.mc_device == ComputeDevice::Cpu ? base : run(route, route.model, 0, cpu);
        const Real dS = 0.01 * route.model.spot;
        scripting::ScriptModelSpec up = route.model, down = route.model;
        up.spot += dS;
        down.spot -= dS;
        const SimulationMCResult su = run(route, up, 0, cpu), sd = run(route, down, 0, cpu);
        const Difference gamma = difference({&su, &centre, &sd}, {1.0, -2.0, 1.0}, dS * dS);
        res.greeks.gamma = gamma.value;
        res.greeks.gamma_std_error = gamma.std_error;

        if (route.discontinuous)
        {
            // The project's bump sizes (utils/greeks.hpp): 1 % spot, 0.1 %
            // vol (every local-vol point: a parallel shift), 0.01 % rate.
            const GreeksBumps bumps;
            const Difference delta = difference({&su, &sd}, {1.0, -1.0}, 2.0 * dS);
            res.greeks.delta = delta.value;
            res.greeks.delta_std_error = delta.std_error;
            auto shifted_vol = [&](Real dv)
            {
                scripting::ScriptModelSpec s = route.model;
                s.vol += dv;
                for (double &v : s.vols)
                    v += dv;
                for (double &v : s.sigma_loc_flat)
                    v += dv;
                for (auto &grid : s.sigma_loc_flats)
                    for (double &v : grid)
                        v += dv;
                return s;
            };
            const SimulationMCResult vu = run(route, shifted_vol(bumps.vega_bump), 0, cpu),
                                     vd = run(route, shifted_vol(-bumps.vega_bump), 0, cpu);
            const Difference vega = difference({&vu, &vd}, {1.0, -1.0}, 2.0 * bumps.vega_bump);
            res.greeks.vega = vega.value;
            res.greeks.vega_std_error = vega.std_error;
            scripting::ScriptModelSpec ru = route.model, rd = route.model;
            ru.rate += bumps.rho_bump;
            rd.rate -= bumps.rho_bump;
            const SimulationMCResult rhu = run(route, ru, 0, cpu), rhd = run(route, rd, 0, cpu);
            const Difference rho = difference({&rhu, &rhd}, {1.0, -1.0}, 2.0 * bumps.rho_bump);
            res.greeks.rho = rho.value;
            res.greeks.rho_std_error = rho.std_error;
        }

        const SimulationMCResult later = run(route, route.model, 1, cpu), sooner = run(route, route.model, -1, cpu);
        const Difference theta = difference({&later, &sooner}, {-1.0, 1.0}, 2.0 / 365.0);
        res.greeks.theta = theta.value;
        res.greeks.theta_std_error = theta.std_error;
        res.diagnostics += route.discontinuous
                               ? " | greeks by central differences with common random numbers (a payoff that "
                                 "jumps: no pathwise derivative)"
                               : " | delta, vega, rho by adjoint differentiation; gamma and theta by central "
                                 "differences with common random numbers";
        return res;
    }

} // namespace quantModeling
