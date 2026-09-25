#include "quantModeling/engines/mc/path_simulation.hpp"

#include "quantModeling/core/sample.hpp"

#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    namespace
    {

        Real normal_draw(Pcg32 &rng)
        {
            return inverse_normal_cdf(uniform01(rng));
        }

        std::vector<Real> make_time_grid(Real ttm, std::size_t n_steps)
        {
            std::vector<Real> grid(n_steps + 1);
            const Real dt = ttm / static_cast<Real>(n_steps);
            for (std::size_t i = 0; i <= n_steps; ++i)
                grid[i] = static_cast<Real>(i) * dt;
            return grid;
        }

    } // namespace

    SimulatedPaths simulate_black_scholes_paths(
        Real spot, Real r, Real q, Real sigma, Real ttm, const PathSimulationSettings &settings)
    {
        SimulatedPaths result;
        if (ttm <= 0.0 || settings.n_paths <= 0 || settings.n_steps == 0)
            return result;

        const std::size_t n = settings.n_steps;
        const Real dt = ttm / static_cast<Real>(n);
        const Real drift = (r - q - 0.5 * sigma * sigma) * dt;
        const Real diffusion = sigma * std::sqrt(dt);

        result.time_grid = make_time_grid(ttm, n);
        result.paths.assign(static_cast<std::size_t>(settings.n_paths), std::vector<Real>(n + 1));

        RngFactory factory(settings.seed);
        Pcg32 rng = factory.make(0);

        for (auto &path : result.paths)
        {
            path[0] = spot;
            for (std::size_t i = 1; i <= n; ++i)
                path[i] = path[i - 1] * std::exp(drift + diffusion * normal_draw(rng));
        }
        return result;
    }

    SimulatedPaths simulate_sabr_paths(
        Real forward, const SABRParams &params, Real ttm, const PathSimulationSettings &settings)
    {
        SimulatedPaths result;
        if (ttm <= 0.0 || settings.n_paths <= 0 || settings.n_steps == 0)
            return result;

        const std::size_t n = settings.n_steps;
        const Real dt = ttm / static_cast<Real>(n);
        const Real sqrt_dt = std::sqrt(dt);
        const Real sqrt_one_minus_rho2 = std::sqrt(std::max(1.0 - params.rho * params.rho, 0.0));

        result.time_grid = make_time_grid(ttm, n);
        result.paths.assign(static_cast<std::size_t>(settings.n_paths), std::vector<Real>(n + 1));

        RngFactory factory(settings.seed);
        Pcg32 rng = factory.make(0);

        for (auto &path : result.paths)
        {
            path[0] = forward;
            Real alpha = params.alpha;

            for (std::size_t i = 1; i <= n; ++i)
            {
                const Real z1 = normal_draw(rng);
                const Real z2_indep = normal_draw(rng);
                const Real z2 = params.rho * z1 + sqrt_one_minus_rho2 * z2_indep;

                const Real f_prev = path[i - 1];
                const Real f_pow_beta = f_prev > 0.0 ? std::pow(f_prev, params.beta) : 0.0;
                const Real f_next = f_prev + alpha * f_pow_beta * sqrt_dt * z1;
                path[i] = std::max(f_next, Real(0.0)); // absorbed at 0 -- F^beta undefined below for non-integer beta

                alpha *= std::exp(-0.5 * params.nu * params.nu * dt + params.nu * sqrt_dt * z2); // exact for alpha's own driftless GBM
            }
        }
        return result;
    }

    SimulatedPaths simulate_model_paths(
        ISimulationModel<Real> &model, Real spot, Real ttm,
        const PathSimulationSettings &settings)
    {
        if (!(ttm > 0.0) || settings.n_steps < 1 || settings.n_paths < 1)
            throw InvalidInput("simulate_model_paths: need ttm > 0, n_steps >= 1 and n_paths >= 1");

        const std::size_t n = settings.n_steps;
        TimeLine timeline(n);
        for (std::size_t i = 0; i < n; ++i)
            timeline[i] = ttm * static_cast<Real>(i + 1) / static_cast<Real>(n);
        const std::vector<SampleDef> defline(n);
        model.init(timeline, defline);

        SimulatedPaths out;
        out.time_grid.reserve(n + 1);
        out.time_grid.push_back(0.0);
        out.time_grid.insert(out.time_grid.end(), timeline.begin(), timeline.end());

        Scenario<Real> scenario;
        allocate_scenario(scenario, defline, model.n_underlyings());
        std::vector<double> gaussians(model.sim_dim());
        Pcg32 rng = RngFactory(settings.seed).make(0);
        NormalBoxMuller bm;
        out.paths.reserve(static_cast<std::size_t>(settings.n_paths));
        for (long long p = 0; p < settings.n_paths; ++p)
        {
            for (double &g : gaussians)
                g = bm(rng);
            model.generate_path(gaussians, scenario);
            std::vector<Real> path;
            path.reserve(n + 1);
            path.push_back(spot);
            for (const Sample<Real> &s : scenario)
                path.push_back(s.spots.front());
            out.paths.push_back(std::move(path));
        }
        return out;
    }

} // namespace quantModeling
