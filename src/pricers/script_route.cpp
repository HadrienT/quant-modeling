#include "quantModeling/pricers/script_route.hpp"

#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/conventions.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/greeks.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{

    int ticks_for(Time t)
    {
        return static_cast<int>(std::lround(t * static_cast<double>(kTicksPerYear)));
    }

    namespace
    {
        /// ACT/D, D chosen so that the route's maturity date is exactly its
        /// maturity in years (ScriptRoute::maturity).
        class RouteDays final : public DayCounter
        {
          public:
            RouteDays(int days, Time maturity)
                : days_(days), maturity_(maturity) {}
            const char *name() const noexcept override { return "Actual/D (script route)"; }
            long day_count(const Date &s, const Date &e) const override { return e - s; }
            Time year_fraction(const Date &s, const Date &e) const override
            {
                const long n = e - s;
                return n == days_ ? maturity_ : static_cast<Time>(n) * (maturity_ / static_cast<Time>(days_));
            }
            /// One day, in years.
            Time day() const { return maturity_ / static_cast<Time>(days_); }

          private:
            int days_;
            Time maturity_;
        };

        RouteDays counter_for(const ScriptRoute &route)
        {
            if (route.maturity > 0.0)
                return RouteDays(std::max(1, ticks_for(route.maturity)), route.maturity);
            return RouteDays(kTicksPerYear, 1.0);
        }

        ValuationContext route_context(const ScriptRoute &route, const RouteDays &counter)
        {
            return ValuationContext{route.valuation, &counter};
        }

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
            const RouteDays counter = counter_for(route);
            const ScriptedProduct<Real> product(route.script(spec, days), route_context(route, counter),
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
        const RouteDays counter = counter_for(route);
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

        // Delta, vega, rho: one adjoint run without the control (the adjoint
        // engines have none: unbiased, wider) -- on the GPU when the request
        // runs there and the device takes it, else on the CPU tape. A greek
        // that sums several parameters (local vol's vega: every grid point)
        // needs the sum's error, which only the CPU tape's batches give: on
        // the GPU it is a central difference instead.
        const bool multi = route.model.spots.size() >= 2;
        bool fd[4] = {false, route.fd_greeks || multi, route.fd_greeks, route.fd_greeks};
        std::string aad_note;
        if (!route.fd_greeks)
        {
            const std::size_t paths = static_cast<std::size_t>(settings.mc_paths > 0 ? settings.mc_paths : 100000);
            const auto seed = static_cast<std::uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);
            const SamplerKind sampler =
                settings.mc_sampler == SamplerKind::Sobol ? SamplerKind::Sobol : SamplerKind::PseudoRandom;
            std::optional<AADSimulResults> gpu;
            if (settings.mc_device != ComputeDevice::Cpu)
            {
                const ScriptedProduct<Real> product(route.script(route.model, 0), route_context(route, counter),
                                                    ScriptSettings{route.fuzzy});
                auto model = scripting::make_script_model<Real>(route.model);
                std::string why;
                gpu = simulate_script_aad_gpu(product, *model, paths, seed, why, sampler, settings.mc_gpus);
                if (!gpu && settings.mc_device == ComputeDevice::Gpu)
                    throw InvalidInput("GPU: " + why);
                aad_note = gpu ? "adjoint risks on the GPU" : "adjoint risks on the CPU (" + why + ")";
            }
            std::optional<Real> value[4], error[4];
            if (gpu)
            {
                int count[4] = {0, 0, 0, 0};
                for (std::size_t j = 0; j < gpu->risks.size(); ++j)
                {
                    const int g = static_cast<int>(greek_of(gpu->risk_labels[j]));
                    if (g == 0)
                        continue;
                    ++count[g];
                    value[g] = gpu->risks[j];
                    if (j < gpu->risk_std_errors.size())
                        error[g] = gpu->risk_std_errors[j];
                }
                for (int g = 1; g < 4; ++g)
                    fd[g] = count[g] != 1;
            }
            else
            {
                const ScriptedProduct<aad::Number> product(route.script(route.model, 0),
                                                           route_context(route, counter),
                                                           ScriptSettings{route.fuzzy});
                auto model = scripting::make_script_model<aad::Number>(route.model);
                std::vector<std::vector<Real>> batches;
                const AADSimulResults aad = simulate_aad(product, *model, paths, seed, first_aad_payoff,
                                                         RngKind::Philox, sampler, &batches);
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
                for (int g = 1; g < 4; ++g)
                {
                    value[g] = total[g];
                    if (batches.size() >= 2)
                        error[g] = acc[g].std_error();
                }
                if (aad_note.empty())
                    aad_note = "adjoint risks on the CPU";
            }
            if (!fd[1])
                res.greeks.delta = value[1], res.greeks.delta_std_error = error[1];
            // (several underlyings: delta per unit of the reference level is a
            // central difference below)
            if (!fd[2])
                res.greeks.vega = value[2], res.greeks.vega_std_error = error[2];
            if (!fd[3])
                res.greeks.rho = value[3], res.greeks.rho_std_error = error[3];
        }

        // The central differences: the request's device and Sobol points
        // (same seed), errors replicate-wise. The project's bump sizes
        // (utils/greeks.hpp): 1 % spot, 0.1 % vol (every local-vol point: a
        // parallel shift), 0.01 % rate, one day of maturity.
        const GreeksBumps bumps;
        const Real reference = route.spot_reference > 0.0 ? route.spot_reference
                                                          : (multi ? route.model.spots.front() : route.model.spot);
        const Real dS = bumps.delta_bump * reference;
        scripting::ScriptModelSpec up = route.model, down = route.model;
        if (multi)
            for (std::size_t i = 0; i < up.spots.size(); ++i)
            {
                up.spots[i] *= 1.0 + bumps.delta_bump;
                down.spots[i] *= 1.0 - bumps.delta_bump;
            }
        else
        {
            up.spot += dS;
            down.spot -= dS;
        }
        const SimulationMCResult su = run(route, up, 0, settings), sd = run(route, down, 0, settings);
        const Difference gamma = difference({&su, &base, &sd}, {1.0, -2.0, 1.0}, dS * dS);
        res.greeks.gamma = gamma.value;
        res.greeks.gamma_std_error = gamma.std_error;
        if (fd[1])
        {
            const Difference delta = difference({&su, &sd}, {1.0, -1.0}, 2.0 * dS);
            res.greeks.delta = delta.value;
            res.greeks.delta_std_error = delta.std_error;
        }

        const SimulationMCResult later = run(route, route.model, kTicksPerDay, settings),
                                 sooner = run(route, route.model, -kTicksPerDay, settings);
        const Difference theta = difference({&later, &sooner}, {-1.0, 1.0}, 2.0 * kTicksPerDay * counter.day());
        res.greeks.theta = theta.value;
        res.greeks.theta_std_error = theta.std_error;

        if (fd[2])
        {
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
            const SimulationMCResult vu = run(route, shifted_vol(bumps.vega_bump), 0, settings),
                                     vd = run(route, shifted_vol(-bumps.vega_bump), 0, settings);
            const Difference vega = difference({&vu, &vd}, {1.0, -1.0}, 2.0 * bumps.vega_bump);
            res.greeks.vega = vega.value;
            res.greeks.vega_std_error = vega.std_error;
        }
        if (fd[3])
        {
            scripting::ScriptModelSpec ru = route.model, rd = route.model;
            ru.rate += bumps.rho_bump;
            rd.rate -= bumps.rho_bump;
            const SimulationMCResult rhu = run(route, ru, 0, settings), rhd = run(route, rd, 0, settings);
            const Difference rho = difference({&rhu, &rhd}, {1.0, -1.0}, 2.0 * bumps.rho_bump);
            res.greeks.rho = rho.value;
            res.greeks.rho_std_error = rho.std_error;
        }

        std::string fd_names;
        const char *names[4] = {"", "delta", "vega", "rho"};
        for (int g = 1; g < 4; ++g)
            if (fd[g])
                fd_names += std::string(names[g]) + ", ";
        res.diagnostics += " | " + (aad_note.empty() ? std::string() : aad_note + "; ") + fd_names +
                           "gamma and theta by central differences with common random numbers";
        return res;
    }

} // namespace quantModeling
