#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    struct SimulationMCResult
    {
        std::vector<std::string> labels;
        std::vector<Real> values;     ///< estimate per label
        std::vector<Real> std_errors; ///< aligned standard errors
        long long n_paths = 0;
        std::string diagnostics;
        std::string device = "cpu"; ///< where the paths ran: "cpu" or "gpu"

        Real npv() const { return values.empty() ? Real(0) : values.front(); }
        Real std_error() const
        {
            return std_errors.empty() ? Real(0) : std_errors.front();
        }
    };

    /**
     * @brief The one Monte-Carlo engine for the timeline architecture.
     *
     * Drives any ISimulatableProduct against any ISimulationModel. Samplers:
     *  - PseudoRandom: PCG32 + Box-Muller or inverse-normal, optional antithetic.
     *  - Sobol: scrambled-Sobol RQMC in settings.mc_rqmc_batches digital-shift
     *    replicates; the reported error is the spread of the batch means
     *    (same scheme as the vanilla BS MC engine).
     *
     * The product returns numeraire-deflated payoffs, so the engine only
     * averages — no discounting here.
     */
    namespace detail
    {
        /// Per-label Welford accumulators, mergeable in the logical-block
        /// tree (engines/mc/logical_blocks.hpp).
        struct LabelStats
        {
            std::vector<WelfordAccumulator> acc;

            void add(const std::vector<Real> &v)
            {
                if (acc.empty())
                    acc.resize(v.size());
                for (std::size_t l = 0; l < v.size(); ++l)
                    acc[l].add(v[l]);
            }
            void merge(const LabelStats &o)
            {
                if (o.acc.empty())
                    return;
                if (acc.empty())
                {
                    acc = o.acc;
                    return;
                }
                for (std::size_t l = 0; l < acc.size(); ++l)
                    acc[l].merge(o.acc[l]);
            }
        };
    } // namespace detail

    template <class T = Real>
    SimulationMCResult simulate(const ISimulatableProduct<T> &product,
                                ISimulationModel<T> &model,
                                const PricingSettings &settings)
    {
        model.init(product.timeline(), product.defline());

        const std::size_t dim = model.sim_dim();
        const auto &labels = product.payoff_labels();
        const std::size_t n_labels = labels.size();

        SimulationMCResult res;
        res.labels = labels;
        res.values.assign(n_labels, Real(0));
        res.std_errors.assign(n_labels, Real(0));

        Scenario<T> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<T> pay(n_labels);
        std::vector<double> gauss(std::max<std::size_t>(dim, 1));

        const int requested = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed =
            static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);

        auto run_payoff = [&](std::vector<WelfordAccumulator> &acc)
        {
            model.generate_path(std::span<const double>(gauss.data(), dim), path);
            product.payoffs(path, pay);
            for (std::size_t l = 0; l < n_labels; ++l)
                acc[l].add(static_cast<Real>(pay[l]));
        };

        if (settings.mc_sampler == SamplerKind::Sobol && dim > 0)
        {
            const int B = std::max(2, settings.mc_rqmc_batches);
            const int per_batch = std::max(1, requested / B);
            std::vector<WelfordAccumulator> batch_means(n_labels);

            // Brownian bridge (blueprint/wp/19-gpu.md §2.4): the point's
            // leading coordinates -- Sobol's best -- go to the terminal values.
            std::optional<BridgedGaussians> bridge;
            if (settings.mc_brownian_bridge)
            {
                const BrownianLayout layout = model.brownian_layout();
                if (layout.covers(dim))
                    bridge.emplace(layout.times, layout.factors, layout.stride);
            }
            std::vector<double> point(bridge ? dim : 0);

            for (int b = 0; b < B; ++b)
            {
                const uint64_t batch_seed =
                    (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) |
                    static_cast<uint64_t>(b);
                SobolSequence sobol(static_cast<int>(dim), batch_seed);
                std::vector<WelfordAccumulator> inner(n_labels);
                for (int p = 0; p < per_batch; ++p)
                {
                    if (bridge)
                    {
                        sobol.next_gaussian(std::span<double>(point.data(), dim));
                        bridge->map(point, std::span<double>(gauss.data(), dim));
                    }
                    else
                    {
                        sobol.next_gaussian(std::span<double>(gauss.data(), dim));
                    }
                    run_payoff(inner);
                }
                for (std::size_t l = 0; l < n_labels; ++l)
                    batch_means[l].add(inner[l].mean);
            }

            for (std::size_t l = 0; l < n_labels; ++l)
            {
                res.values[l] = batch_means[l].mean;
                res.std_errors[l] = batch_means[l].std_error();
            }
            res.n_paths = static_cast<long long>(per_batch) * B;
            res.diagnostics = "SimulationMCEngine + Sobol RQMC (" +
                              std::to_string(B) + " batches, dim=" +
                              std::to_string(dim) + ")" +
                              (bridge ? " + Brownian bridge" : "");
            return res;
        }

        // ── Counter-based (blueprint/wp/19-gpu.md §2.3, §7) ──────────────
        // Unit u is path u -- or, antithetic, the pair (z, −z) of path u,
        // its payoffs averaged -- with draw j = Φ⁻¹(Philox(seed, u, j)); the
        // units are reduced by the GPU's tree. The GPU script engine
        // (engines/mc/script_engine.hpp) runs the same units, draws and tree.
        if (settings.mc_rng == RngKind::Philox && dim > 0)
        {
            const bool anti = settings.mc_antithetic;
            const auto n_units = static_cast<uint64_t>(anti ? (requested + 1) / 2 : requested);
            std::vector<Real> out(n_labels), mirror_pay(n_labels);
            auto unit = [&](uint64_t u) -> std::vector<Real>
            {
                for (std::size_t j = 0; j < dim; ++j)
                    gauss[j] = inverse_normal_cdf(philox_uniform(seed, u, static_cast<uint32_t>(j)));
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                for (std::size_t l = 0; l < n_labels; ++l)
                    out[l] = static_cast<Real>(pay[l]);
                if (!anti)
                    return out;
                for (std::size_t j = 0; j < dim; ++j)
                    gauss[j] = -gauss[j];
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                for (std::size_t l = 0; l < n_labels; ++l)
                    out[l] = 0.5 * (out[l] + static_cast<Real>(pay[l]));
                return out;
            };
            const detail::LabelStats stats = mc::reduce_logical_blocks<detail::LabelStats>(n_units, unit);
            for (std::size_t l = 0; l < n_labels && l < stats.acc.size(); ++l)
            {
                res.values[l] = stats.acc[l].mean;
                res.std_errors[l] = stats.acc[l].std_error();
            }
            res.n_paths = static_cast<long long>(n_units) * (anti ? 2 : 1);
            res.diagnostics = std::string("SimulationMCEngine + Philox") + (anti ? " + antithetic" : "");
            return res;
        }

        // ── Pseudo-random ────────────────────────────────────────────────
        std::vector<WelfordAccumulator> acc(n_labels);
        Pcg32 rng = RngFactory(seed).make(0);
        NormalBoxMuller bm;
        const bool inv_normal = settings.mc_gaussian == GaussianKind::InverseNormal;
        const bool antithetic = settings.mc_antithetic && dim > 0;

        std::vector<double> mirror(std::max<std::size_t>(dim, 1));
        for (int p = 0; p < requested; ++p)
        {
            if (antithetic && (p & 1))
            {
                for (std::size_t d = 0; d < dim; ++d)
                    gauss[d] = -mirror[d];
            }
            else
            {
                for (std::size_t d = 0; d < dim; ++d)
                    gauss[d] = inv_normal ? inverse_normal_cdf(uniform01(rng))
                                          : bm(rng);
                if (antithetic)
                    std::copy_n(gauss.data(), dim, mirror.data());
            }
            run_payoff(acc);
        }

        for (std::size_t l = 0; l < n_labels; ++l)
        {
            res.values[l] = acc[l].mean;
            res.std_errors[l] = acc[l].std_error();
        }
        res.n_paths = requested;
        res.diagnostics = std::string("SimulationMCEngine + pseudo-random") +
                          (inv_normal ? " (inverse normal)" : " (Box-Muller)") +
                          (antithetic ? " + antithetic" : "");
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_HPP
