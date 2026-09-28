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
#include "quantModeling/engines/mc/path_draws.hpp"
#include "quantModeling/engines/mc/spot_controls.hpp"
#include "quantModeling/utils/variance_reduction/multi_control.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

    namespace detail
    {
        /// Per-label regression accumulators (payoff, spot controls), for the
        /// logical-block tree.
        struct ControlStats
        {
            using Sample = MultiControlAccumulator::Sample;
            std::vector<MultiControlAccumulator> acc;

            void add(const std::vector<Sample> &v)
            {
                if (acc.empty())
                    acc.resize(v.size());
                for (std::size_t l = 0; l < v.size(); ++l)
                    acc[l].add(v[l]);
            }
            void merge(const ControlStats &o)
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

        /// Per-label within-stratum control accumulators (stratified runs).
        struct StratControlStats
        {
            std::vector<StratifiedControlAccumulator> acc;

            void add(const std::vector<MultiControlAccumulator::Sample> &v)
            {
                if (acc.empty())
                    acc.resize(v.size());
                for (std::size_t l = 0; l < v.size(); ++l)
                    acc[l].add(v[l]);
            }
            void merge(const StratControlStats &o)
            {
                if (o.acc.empty())
                    return;
                if (acc.empty())
                {
                    acc = o.acc;
                    for (auto &a : acc)
                        a.has_last = 0;
                    return;
                }
                for (std::size_t l = 0; l < acc.size(); ++l)
                    acc[l].merge(o.acc[l]);
            }
        };

        /// "variance / 7.3" -- the measured reduction of a controlled run.
        inline std::string variance_ratio_note(const MultiControlAccumulator::Estimate &e)
        {
            if (e.std_error <= 0.0 || e.plain_std_error <= 0.0)
                return "";
            const Real ratio = (e.plain_std_error * e.plain_std_error) / (e.std_error * e.std_error);
            char buf[64];
            std::snprintf(buf, sizeof buf, ", variance / %.1f", ratio);
            return buf;
        }
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

        // ── Generic control variates (blueprint/wp/19-gpu.md §2.5) ─────────
        mc::SpotControlSet controls;
        if (settings.mc_spot_control)
            controls = mc::choose_spot_controls(product.timeline(), model);
        const int k = controls.sel.n;
        const bool cv = k > 0;
        using CvSample = MultiControlAccumulator::Sample;
        /// The path just generated: controls into s.v[1..k].
        auto read_controls = [&](CvSample &s)
        {
            for (int c = 0; c < k; ++c)
            {
                const Sample<T> &smp = path[static_cast<std::size_t>(controls.sel.event[c])];
                s.v[1 + c] = static_cast<Real>(smp.spots[static_cast<std::size_t>(controls.sel.asset[c])]) /
                             static_cast<Real>(smp.numeraire);
            }
        };
        std::string cv_note;
        auto finish_cv = [&](const std::vector<MultiControlAccumulator> &acc)
        {
            for (std::size_t l = 0; l < n_labels && l < acc.size(); ++l)
            {
                const auto e = acc[l].estimate(controls.mean, k);
                res.values[l] = e.value;
                res.std_errors[l] = e.std_error;
                if (l == 0)
                    cv_note = " + spot control (" + std::to_string(e.used) + " of " + std::to_string(k) + " kept" +
                              detail::variance_ratio_note(e) + ")";
            }
        };
        const std::string cv_off =
            settings.mc_spot_control && !cv ? " (no spot control: " + controls.why_none + ")" : "";

        auto run_payoff = [&](std::vector<WelfordAccumulator> &acc)
        {
            model.generate_path(std::span<const double>(gauss.data(), dim), path);
            product.payoffs(path, pay);
            for (std::size_t l = 0; l < n_labels; ++l)
                acc[l].add(static_cast<Real>(pay[l]));
        };
        auto run_payoff_cv = [&](std::vector<MultiControlAccumulator> &acc)
        {
            model.generate_path(std::span<const double>(gauss.data(), dim), path);
            product.payoffs(path, pay);
            CvSample smp;
            read_controls(smp);
            for (std::size_t l = 0; l < n_labels; ++l)
            {
                smp.v[0] = static_cast<Real>(pay[l]);
                acc[l].add(smp);
            }
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
                std::vector<MultiControlAccumulator> inner_cv(cv ? n_labels : 0);
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
                    if (cv)
                        run_payoff_cv(inner_cv);
                    else
                        run_payoff(inner);
                }
                // A controlled replicate is its own regression estimate;
                // the replicates stay i.i.d.
                for (std::size_t l = 0; l < n_labels; ++l)
                    batch_means[l].add(cv ? inner_cv[l].estimate(controls.mean, k).value : inner[l].mean);
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
                              (bridge ? " + Brownian bridge" : "") + (cv ? " + spot control" : "") + cv_off;
            return res;
        }

        // ── Counter-based (blueprint/wp/19-gpu.md §2.3, §7) ──────────────
        // Unit u is path u -- or, antithetic, the pair (z, −z) of path u,
        // its payoffs averaged -- with draw j = Φ⁻¹(Philox(seed, u, j)); the
        // units are reduced by the GPU's tree. The GPU script engine
        // (engines/mc/script_engine.hpp) runs the same units, draws and tree.
        //
        // Stratified (§2.5, Glasserman §4.3): the first Brownian factor's
        // terminal value in stratum i of m, the path filled in by the
        // conditional bridge (engines/mc/path_draws.hpp); B independent
        // replicates of m strata give the error. Always Philox.
        const bool stratified = settings.mc_sampler == SamplerKind::Stratified && dim > 0;
        if ((settings.mc_rng == RngKind::Philox || stratified) && dim > 0)
        {
            const bool anti = settings.mc_antithetic;
            const auto n_units = static_cast<uint64_t>(anti ? (requested + 1) / 2 : requested);

            mc::PathDraws draws;
            draws.seed = seed;
            BrownianLayout layout;
            int B = 1;
            uint64_t per_rep = n_units;
            if (stratified)
            {
                layout = model.brownian_layout();
                if (!layout.covers(dim))
                    throw InvalidInput("stratified sampling needs a model that describes its Brownian increments "
                                       "(this one does not): use pseudo-random or Sobol");
                B = std::max(2, settings.mc_rqmc_batches);
                per_rep = std::max<uint64_t>(1, n_units / static_cast<uint64_t>(B));
                draws.stride = static_cast<int>(layout.stride);
                draws.strata = per_rep;
                draws.times = layout.times.data();
                draws.n_steps = static_cast<int>(layout.times.size());
            }
            auto fill = [&](uint64_t u, uint64_t stratum)
            {
                if (!stratified)
                {
                    for (std::size_t j = 0; j < dim; ++j)
                        gauss[j] = inverse_normal_cdf(philox_uniform(seed, u, static_cast<uint32_t>(j)));
                    return;
                }
                draws.begin(u, stratum);
                for (int d = 0; d < draws.n_steps; ++d)
                    for (int f = 0; f < draws.stride; ++f)
                        gauss[static_cast<std::size_t>(d * draws.stride + f)] = draws(d, f);
            };
            auto mirror = [&]()
            {
                for (std::size_t j = 0; j < dim; ++j)
                    gauss[j] = -gauss[j];
            };

            std::vector<Real> out(n_labels);
            std::vector<CvSample> out_cv(cv ? n_labels : 0);
            uint64_t rep_first = 0; // replicate b covers units [b m, (b + 1) m)
            // Stratified: unit u of the replicate is in stratum_of(u) -- each
            // reducing thread's successive paths in neighbouring strata.
            auto stratum = [&](uint64_t u)
            { return stratified ? mc::stratum_of(u, per_rep) : u; };
            auto unit = [&](uint64_t u) -> std::vector<Real>
            {
                fill(rep_first + u, stratum(u));
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                for (std::size_t l = 0; l < n_labels; ++l)
                    out[l] = static_cast<Real>(pay[l]);
                if (!anti)
                    return out;
                mirror();
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                for (std::size_t l = 0; l < n_labels; ++l)
                    out[l] = 0.5 * (out[l] + static_cast<Real>(pay[l]));
                return out;
            };
            auto unit_cv = [&](uint64_t u) -> std::vector<CvSample>
            {
                fill(rep_first + u, stratum(u));
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                CvSample base;
                read_controls(base);
                for (std::size_t l = 0; l < n_labels; ++l)
                {
                    out_cv[l] = base;
                    out_cv[l].v[0] = static_cast<Real>(pay[l]);
                }
                if (!anti)
                    return out_cv;
                mirror();
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, pay);
                CvSample other;
                read_controls(other);
                for (std::size_t l = 0; l < n_labels; ++l)
                {
                    other.v[0] = static_cast<Real>(pay[l]);
                    for (int i = 0; i <= k; ++i)
                        out_cv[l].v[i] = 0.5 * (out_cv[l].v[i] + other.v[i]);
                }
                return out_cv;
            };

            std::string note = "SimulationMCEngine + Philox";
            if (!stratified)
            {
                if (cv)
                    finish_cv(mc::reduce_logical_blocks<detail::ControlStats>(n_units, unit_cv).acc);
                else
                {
                    const detail::LabelStats stats = mc::reduce_logical_blocks<detail::LabelStats>(n_units, unit);
                    for (std::size_t l = 0; l < n_labels && l < stats.acc.size(); ++l)
                    {
                        res.values[l] = stats.acc[l].mean;
                        res.std_errors[l] = stats.acc[l].std_error();
                    }
                }
                res.n_paths = static_cast<long long>(n_units) * (anti ? 2 : 1);
            }
            else
            {
                std::vector<WelfordAccumulator> reps(n_labels);
                for (int b = 0; b < B; ++b)
                {
                    rep_first = static_cast<uint64_t>(b) * per_rep;
                    if (cv)
                    {
                        // The within-stratum slope (StratifiedControlAccumulator).
                        const auto st = mc::reduce_logical_blocks<detail::StratControlStats>(per_rep, unit_cv);
                        for (std::size_t l = 0; l < n_labels && l < st.acc.size(); ++l)
                            reps[l].add(st.acc[l].estimate(controls.mean, k));
                    }
                    else
                    {
                        const auto st = mc::reduce_logical_blocks<detail::LabelStats>(per_rep, unit);
                        for (std::size_t l = 0; l < n_labels && l < st.acc.size(); ++l)
                            reps[l].add(st.acc[l].mean);
                    }
                }
                for (std::size_t l = 0; l < n_labels; ++l)
                {
                    res.values[l] = reps[l].mean;
                    res.std_errors[l] = reps[l].std_error();
                }
                res.n_paths = static_cast<long long>(per_rep) * B * (anti ? 2 : 1);
                note += " + stratified W(T) (" + std::to_string(B) + " replicates of " + std::to_string(per_rep) +
                        " strata)";
                cv_note = cv ? " + spot control (within-stratum slope)" : "";
            }
            res.diagnostics = note + (anti ? " + antithetic" : "") + cv_note + cv_off;
            return res;
        }

        // ── Pseudo-random ────────────────────────────────────────────────
        std::vector<WelfordAccumulator> acc(n_labels);
        std::vector<MultiControlAccumulator> acc_cv(cv ? n_labels : 0);
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
            if (cv)
                run_payoff_cv(acc_cv);
            else
                run_payoff(acc);
        }

        if (cv)
            finish_cv(acc_cv);
        else
            for (std::size_t l = 0; l < n_labels; ++l)
            {
                res.values[l] = acc[l].mean;
                res.std_errors[l] = acc[l].std_error();
            }
        res.n_paths = requested;
        res.diagnostics = std::string("SimulationMCEngine + pseudo-random") +
                          (inv_normal ? " (inverse normal)" : " (Box-Muller)") +
                          (antithetic ? " + antithetic" : "") + cv_note + cv_off;
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_HPP
