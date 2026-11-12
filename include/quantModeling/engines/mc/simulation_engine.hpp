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
#include "quantModeling/engines/mc/importance_sampling.hpp"
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
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
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
        int gpus = 0;               ///< GPUs the paths were shared between
        /// Sobol RQMC (CPU or GPU) and stratified replicates: the first
        /// label's estimate in each replicate, in replicate order (empty
        /// otherwise).
        /// Two runs with the same seed share their points replicate by
        /// replicate, so the spread of their replicate-wise differences is
        /// the error of a finite difference with common random numbers.
        std::vector<Real> replicates;
        /// The first label's estimated discretisation bias (WP 26 §9.4),
        /// filled by simulate_script when asked (PricingSettings::
        /// mc_bias_estimate) or by its multilevel route; absent otherwise.
        std::optional<Real> discretisation_bias;

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

    /// One simulation run; `theta` (per Brownian factor) is an importance-
    /// sampling drift, or null. simulate() below decides whether to use one.
    ///
    /// With a `pool` (blueprint/wp/26-performance.md, P1e), the Sobol
    /// replicates and the counter-based logical blocks run as tasks, each
    /// thread on its own clone of the model, its own payoff state and its own
    /// buffers; the results are folded in replicate or block order, so they
    /// are the same bits on any number of threads. The pseudo-random PCG32
    /// run is one stream and stays on the calling thread, as does a product
    /// whose payoffs() cannot run concurrently (make_payoff_state() null) and
    /// a differentiable run (T != Real).
    template <class T>
    SimulationMCResult simulate_impl(const ISimulatableProduct<T> &product, ISimulationModel<T> &model,
                                     const PricingSettings &settings, const std::vector<double> *theta,
                                     ThreadPool *pool = nullptr)
    {
        model.init(product.timeline(), product.defline());

        const std::size_t dim = model.sim_dim();
        // Sobol is the default (WP 26 P1c); beyond the generator's dimensions
        // the path runs pseudo-random, and says so.
        if (settings.mc_sampler == SamplerKind::Sobol && dim > static_cast<std::size_t>(sobol_detail::kMaxDimension))
        {
            PricingSettings pseudo = settings;
            pseudo.mc_sampler = SamplerKind::PseudoRandom;
            SimulationMCResult res = simulate_impl<T>(product, model, pseudo, theta, pool);
            res.diagnostics += " (pseudo-random: " + std::to_string(dim) + " draws per path, more than the " +
                               std::to_string(sobol_detail::kMaxDimension) + " Sobol dimensions)";
            return res;
        }
        const auto &labels = product.payoff_labels();
        const std::size_t n_labels = labels.size();

        SimulationMCResult res;
        res.labels = labels;
        res.values.assign(n_labels, Real(0));
        res.std_errors.assign(n_labels, Real(0));

        const int requested = settings.mc_paths > 0 ? settings.mc_paths : 100000;
        const auto seed =
            static_cast<uint64_t>(settings.mc_seed > 0 ? settings.mc_seed : 1);

        // ── Generic control variates (blueprint/wp/19-gpu.md §2.5) ─────────
        mc::SpotControlSet controls;
        if (settings.mc_spot_control)
            controls = mc::choose_spot_controls(product.timeline(), model);
        // The product's own analytic control (WP 26 P2), after the spots:
        // one more regressor with its closed-form mean.
        int product_control = -1;
        {
            Real mean = 0.0;
            if (settings.mc_control_variate && product.analytic_control(mean) &&
                controls.sel.n < mc::SpotControls::kMax)
            {
                product_control = controls.sel.n++;
                controls.mean[product_control] = mean;
            }
        }
        const int k = controls.sel.n;
        const bool cv = k > 0;
        using CvSample = MultiControlAccumulator::Sample;
        using PayoffState = typename ISimulatableProduct<T>::PayoffState;

        // ── Workers: worker 0 is the calling thread, on `model` itself;
        // worker n (1..pool threads) on a clone of it.
        struct Worker
        {
            std::unique_ptr<ISimulationModel<T>> clone;
            ISimulationModel<T> *model = nullptr;
            std::unique_ptr<PayoffState> state;
            Scenario<T> path;
            std::vector<T> pay;
            std::vector<double> gauss, point;
            double path_weight = 1.0;
            std::optional<BridgedGaussians> bridge;
            mc::PathDraws draws;
            std::vector<Real> out;
            std::vector<CvSample> out_cv;
            Real control = 0.0; ///< the product's analytic control on the last path
            // The batched interpreter's (P1a): its paths, their weights and
            // payoffs, and one logical block's samples.
            std::unique_ptr<PayoffState> batch_state;
            std::vector<Scenario<T>> batch_paths;
            std::vector<const Scenario<T> *> batch_ptrs;
            std::vector<double> batch_weight;
            std::vector<T> batch_pay;
            int batch_fill = 0; ///< paths waiting in batch_paths
            std::vector<std::vector<Real>> block_samples;
        };
        // P1a: several paths per call of the product, where it can (one
        // payoff, double, no control variates); the same bits as path by path.
        const int batch = std::is_same_v<T, Real> && settings.mc_batch && n_labels == 1 && !cv
                              ? product.batch_width()
                              : 0;
        bool parallel = pool != nullptr && pool->num_threads() > 0 && std::is_same_v<T, Real> &&
                        ThreadPool::thread_num() == 0;
        std::unique_ptr<PayoffState> first_state = parallel ? product.make_payoff_state() : nullptr;
        if (!first_state)
            parallel = false;
        std::vector<Worker> workers(parallel ? pool->num_threads() + 1 : 1);
        for (std::size_t i = 0; i < workers.size(); ++i)
        {
            Worker &w = workers[i];
            if (i == 0)
                w.model = &model;
            else
            {
                w.clone = model.clone();
                w.clone->init(product.timeline(), product.defline());
                w.model = w.clone.get();
            }
            w.state = i == 0 ? std::move(first_state) : product.make_payoff_state();
            allocate_scenario(w.path, product.defline(), w.model->n_underlyings());
            w.pay.resize(n_labels);
            w.gauss.assign(std::max<std::size_t>(dim, 1), 0.0);
            w.out.resize(n_labels);
            w.out_cv.resize(cv ? n_labels : 0);
            if (batch > 0)
            {
                w.batch_state = product.make_batch_state();
                w.batch_paths.resize(static_cast<std::size_t>(batch));
                for (Scenario<T> &path : w.batch_paths)
                    allocate_scenario(path, product.defline(), w.model->n_underlyings());
                for (const Scenario<T> &path : w.batch_paths)
                    w.batch_ptrs.push_back(&path);
                w.batch_weight.assign(static_cast<std::size_t>(batch), 1.0);
                w.batch_pay.assign(static_cast<std::size_t>(batch), T(0));
            }
        }
        auto me = [&]() -> Worker &
        { return workers[parallel ? ThreadPool::thread_num() : 0]; };
        ThreadPool *task_pool = parallel ? pool : nullptr;
        /// task(i) for i in [0, n): on the pool, every task finished before
        /// the first error is rethrown.
        auto run_tasks = [&](int n, const auto &task)
        {
            if (!parallel)
            {
                for (int i = 0; i < n; ++i)
                    task(i);
                return;
            }
            std::vector<TaskHandle> handles;
            handles.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                handles.push_back(pool->spawn_task([&task, i]()
                                                   {
                    task(i);
                    return true; }));
            pool->active_wait_all(handles);
        };

        /// The worker's path from its draws, then its payoffs.
        auto evaluate = [&](Worker &w)
        {
            w.model->generate_path(std::span<const double>(w.gauss.data(), dim), w.path);
            if (w.state)
                product.payoffs_with_state(w.path, w.pay, *w.state);
            else
                product.payoffs(w.path, w.pay);
            if (product_control >= 0)
                w.control = product.control_value(w.state.get());
        };
        /// The path just generated: controls into s.v[1..k].
        auto read_controls = [&](const Worker &w, CvSample &s)
        {
            for (int c = 0; c < k; ++c)
            {
                if (c == product_control)
                {
                    s.v[1 + c] = w.control;
                    continue;
                }
                const Sample<T> &smp = w.path[static_cast<std::size_t>(controls.sel.event[c])];
                s.v[1 + c] = static_cast<Real>(smp.spots[static_cast<std::size_t>(controls.sel.asset[c])]) /
                             static_cast<Real>(smp.numeraire);
            }
        };
        std::string cv_note;
        /// What the controls are, for the diagnostics.
        auto cv_label = [&]() -> std::string
        {
            if (product_control < 0)
                return " + spot control";
            return k > 1 ? " + analytic and spot controls" : " + analytic control";
        };
        auto finish_cv = [&](const std::vector<MultiControlAccumulator> &acc)
        {
            for (std::size_t l = 0; l < n_labels && l < acc.size(); ++l)
            {
                const auto e = acc[l].estimate(controls.mean, k);
                res.values[l] = e.value;
                res.std_errors[l] = e.std_error;
                if (l == 0)
                    cv_note = cv_label() + " (" + std::to_string(e.used) + " of " + std::to_string(k) + " kept" +
                              detail::variance_ratio_note(e) + ")";
            }
        };
        const std::string cv_off =
            settings.mc_spot_control && !cv ? " (no spot control: " + controls.why_none + ")" : "";
        auto threads_note = [&]()
        { return parallel ? " on " + std::to_string(workers.size()) + " threads" : std::string(); };

        // ── Importance sampling (mc/importance_sampling.hpp): Brownian factor
        // f drifts by theta_f; each path carries its likelihood ratio, which
        // weights the payoff and the controls alike.
        BrownianLayout is_layout;
        if (theta)
        {
            is_layout = model.brownian_layout();
            if (!is_layout.covers(dim) || theta->size() > is_layout.factors)
                throw InvalidInput("importance sampling needs a model that describes its Brownian increments");
        }
        /// w.gauss holds a path's unshifted draws: shift them, set w.path_weight.
        auto shift_gauss = [&](Worker &w)
        {
            if (!theta)
                return;
            double log_w = 0.0, prev = 0.0;
            for (std::size_t d = 0; d < is_layout.times.size(); ++d)
            {
                const double sq = std::sqrt(is_layout.times[d] - prev);
                prev = is_layout.times[d];
                for (std::size_t f = 0; f < theta->size(); ++f)
                {
                    double &z = w.gauss[d * is_layout.stride + f];
                    const double mu = (*theta)[f] * sq;
                    log_w += -mu * z - 0.5 * mu * mu;
                    z += mu;
                }
            }
            w.path_weight = std::exp(log_w);
        };

        auto run_payoff = [&](Worker &w, std::vector<WelfordAccumulator> &acc)
        {
            evaluate(w);
            for (std::size_t l = 0; l < n_labels; ++l)
                acc[l].add(theta ? static_cast<Real>(w.pay[l]) * w.path_weight : static_cast<Real>(w.pay[l]));
        };
        // P1a: the paths of a sequence (Sobol points) in batches; their
        // payoffs added in the same order as run_payoff would.
        auto flush_batch = [&](Worker &w, std::vector<WelfordAccumulator> &acc)
        {
            const int np = w.batch_fill;
            if (np == 0)
                return;
            w.batch_fill = 0; // first: a payoff that throws leaves no stale count for the next task
            product.payoffs_batch(w.batch_ptrs.data(), np, w.batch_pay.data(), *w.batch_state);
            for (int i = 0; i < np; ++i)
            {
                const Real p = static_cast<Real>(w.batch_pay[static_cast<std::size_t>(i)]);
                acc[0].add(theta ? p * w.batch_weight[static_cast<std::size_t>(i)] : p);
            }
        };
        auto batch_path = [&](Worker &w, std::vector<WelfordAccumulator> &acc)
        {
            w.model->generate_path(std::span<const double>(w.gauss.data(), dim),
                                   w.batch_paths[static_cast<std::size_t>(w.batch_fill)]);
            w.batch_weight[static_cast<std::size_t>(w.batch_fill++)] = w.path_weight;
            if (w.batch_fill == batch)
                flush_batch(w, acc);
        };
        auto run_payoff_cv = [&](Worker &w, std::vector<MultiControlAccumulator> &acc)
        {
            evaluate(w);
            CvSample smp;
            read_controls(w, smp);
            if (theta)
                for (int c = 0; c < k; ++c)
                    smp.v[1 + c] *= w.path_weight;
            for (std::size_t l = 0; l < n_labels; ++l)
            {
                smp.v[0] = theta ? static_cast<Real>(w.pay[l]) * w.path_weight : static_cast<Real>(w.pay[l]);
                acc[l].add(smp);
            }
        };

        if (settings.mc_sampler == SamplerKind::Sobol && dim > 0)
        {
            const int B = std::max(2, settings.mc_rqmc_batches);
            const int per_batch = static_cast<int>(sobol_points_per_replicate(static_cast<std::size_t>(requested),
                                                                              static_cast<std::size_t>(B)));

            // Brownian bridge (blueprint/wp/19-gpu.md §2.4): the point's
            // leading coordinates -- Sobol's best -- go to the terminal values.
            bool bridged = false;
            if (settings.mc_brownian_bridge)
            {
                const BrownianLayout layout = model.brownian_layout();
                if (layout.covers(dim))
                {
                    bridged = true;
                    for (Worker &w : workers)
                    {
                        w.bridge.emplace(layout.times, layout.factors, layout.stride);
                        w.point.assign(dim, 0.0);
                    }
                }
            }

            // Replicate b's estimate, label by label; folded in b order.
            std::vector<std::vector<Real>> estimates(static_cast<std::size_t>(B), std::vector<Real>(n_labels));
            run_tasks(B,
                      [&](int b)
                      {
                          Worker &w = me();
                          const uint64_t batch_seed =
                              (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) |
                              static_cast<uint64_t>(b);
                          SobolSequence sobol(static_cast<int>(dim), batch_seed, settings.mc_sobol_owen);
                          std::vector<WelfordAccumulator> inner(n_labels);
                          std::vector<MultiControlAccumulator> inner_cv(cv ? n_labels : 0);
                          w.batch_fill = 0;
                          for (int p = 0; p < per_batch; ++p)
                          {
                              if (w.bridge)
                              {
                                  sobol.next_gaussian(std::span<double>(w.point.data(), dim));
                                  w.bridge->map(w.point, std::span<double>(w.gauss.data(), dim));
                              }
                              else
                              {
                                  sobol.next_gaussian(std::span<double>(w.gauss.data(), dim));
                              }
                              shift_gauss(w);
                              if (cv)
                                  run_payoff_cv(w, inner_cv);
                              else if (batch > 0)
                                  batch_path(w, inner);
                              else
                                  run_payoff(w, inner);
                          }
                          if (batch > 0)
                              flush_batch(w, inner);
                          // A controlled replicate is its own regression
                          // estimate; the replicates stay i.i.d.
                          for (std::size_t l = 0; l < n_labels; ++l)
                              estimates[static_cast<std::size_t>(b)][l] =
                                  cv ? inner_cv[l].estimate(controls.mean, k).value : inner[l].mean;
                      });
            std::vector<WelfordAccumulator> batch_means(n_labels);
            for (int b = 0; b < B; ++b)
                for (std::size_t l = 0; l < n_labels; ++l)
                    batch_means[l].add(estimates[static_cast<std::size_t>(b)][l]);
            res.replicates.clear();
            for (int b = 0; b < B && n_labels > 0; ++b)
                res.replicates.push_back(estimates[static_cast<std::size_t>(b)][0]);

            for (std::size_t l = 0; l < n_labels; ++l)
            {
                res.values[l] = batch_means[l].mean;
                res.std_errors[l] = batch_means[l].std_error();
            }
            res.n_paths = static_cast<long long>(per_batch) * B;
            res.diagnostics = "SimulationMCEngine + Sobol RQMC (" +
                              std::to_string(B) + " batches of " + std::to_string(per_batch) + " points, dim=" +
                              std::to_string(dim) + ")" +
                              (bridged ? " + Brownian bridge" : "") + (settings.mc_sobol_owen ? " + Owen scrambling" : "") +
                              (cv ? cv_label() : "") + cv_off + threads_note();
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
        if ((settings.mc_rng == RngKind::Philox || stratified || theta) && dim > 0)
        {
            const bool anti = settings.mc_antithetic;
            const auto n_units = static_cast<uint64_t>(anti ? (requested + 1) / 2 : requested);

            mc::PathDraws draws;
            draws.seed = seed;
            BrownianLayout layout;
            int B = 1;
            uint64_t per_rep = n_units;
            const bool shaped = stratified || theta; // draws built step by step by PathDraws
            if (shaped)
            {
                layout = model.brownian_layout();
                if (!layout.covers(dim))
                    throw InvalidInput("stratified sampling needs a model that describes its Brownian increments "
                                       "(this one does not): use pseudo-random or Sobol");
                draws.stride = static_cast<int>(layout.stride);
                draws.times = layout.times.data();
                draws.n_steps = static_cast<int>(layout.times.size());
            }
            if (stratified)
            {
                B = std::max(2, settings.mc_rqmc_batches);
                per_rep = std::max<uint64_t>(1, n_units / static_cast<uint64_t>(B));
                draws.strata = per_rep;
            }
            if (theta)
            {
                draws.theta = theta->data();
                draws.n_theta = static_cast<int>(theta->size());
            }
            for (Worker &w : workers)
                w.draws = draws; // PathDraws keeps the unit being drawn
            /// The unit's draws; sign = -1 is its antithetic mirror (with a
            /// drift mu: mu - x, not -(mu + x)).
            auto fill = [&](Worker &w, uint64_t u, uint64_t stratum, double sign)
            {
                if (!shaped)
                {
                    for (std::size_t j = 0; j < dim; ++j)
                        w.gauss[j] = sign * inverse_normal_cdf(philox_uniform(seed, u, static_cast<uint32_t>(j)));
                    return;
                }
                w.draws.begin(u, stratum);
                for (int d = 0; d < w.draws.n_steps; ++d)
                    for (int f = 0; f < w.draws.stride; ++f)
                        w.gauss[static_cast<std::size_t>(d * w.draws.stride + f)] = w.draws.draw(d, f, sign);
                w.path_weight = std::exp(w.draws.log_weight);
            };

            uint64_t rep_first = 0; // replicate b covers units [b m, (b + 1) m)
            // Stratified: unit u of the replicate is in stratum_of(u) -- each
            // reducing thread's successive paths in neighbouring strata.
            auto stratum = [&](uint64_t u)
            { return stratified ? mc::stratum_of(u, per_rep) : u; };
            // The mirror: -z, or with a drift the draws redone with sign -1.
            auto mirror = [&](Worker &w, uint64_t u)
            {
                if (theta)
                    return fill(w, rep_first + u, stratum(u), -1.0);
                for (std::size_t j = 0; j < dim; ++j)
                    w.gauss[j] = -w.gauss[j];
            };
            auto weighted = [&](const Worker &w, const T &p)
            { return theta ? static_cast<Real>(p) * w.path_weight : static_cast<Real>(p); };
            auto unit = [&](uint64_t u) -> std::vector<Real>
            {
                Worker &w = me();
                fill(w, rep_first + u, stratum(u), 1.0);
                evaluate(w);
                for (std::size_t l = 0; l < n_labels; ++l)
                    w.out[l] = weighted(w, w.pay[l]);
                if (!anti)
                    return w.out;
                mirror(w, u);
                evaluate(w);
                for (std::size_t l = 0; l < n_labels; ++l)
                    w.out[l] = 0.5 * (w.out[l] + weighted(w, w.pay[l]));
                return w.out;
            };
            // P1a: a logical block's units, `batch` paths per call of the
            // product (a unit and its mirror side by side); each unit's
            // sample computed as `unit` computes it.
            auto fill_block = [&](uint64_t base, uint64_t count) -> const std::vector<Real> *
            {
                Worker &w = me();
                if (w.block_samples.size() < mc::LogicalBlocks::kUnitsPerBlock)
                    w.block_samples.assign(mc::LogicalBlocks::kUnitsPerBlock, std::vector<Real>(1));
                const int per_call = anti ? batch / 2 : batch;
                for (uint64_t u0 = 0; u0 < count; u0 += static_cast<uint64_t>(per_call))
                {
                    const int nu = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(per_call), count - u0));
                    int np = 0;
                    auto generate = [&]()
                    {
                        w.model->generate_path(std::span<const double>(w.gauss.data(), dim),
                                               w.batch_paths[static_cast<std::size_t>(np)]);
                        w.batch_weight[static_cast<std::size_t>(np++)] = w.path_weight;
                    };
                    for (int j = 0; j < nu; ++j)
                    {
                        const uint64_t u = base + u0 + static_cast<uint64_t>(j);
                        fill(w, rep_first + u, stratum(u), 1.0);
                        generate();
                        if (anti)
                        {
                            mirror(w, u);
                            generate();
                        }
                    }
                    product.payoffs_batch(w.batch_ptrs.data(), np, w.batch_pay.data(), *w.batch_state);
                    auto value = [&](int i)
                    {
                        const Real p = static_cast<Real>(w.batch_pay[static_cast<std::size_t>(i)]);
                        return theta ? p * w.batch_weight[static_cast<std::size_t>(i)] : p;
                    };
                    for (int j = 0; j < nu; ++j)
                    {
                        Real &out = w.block_samples[static_cast<std::size_t>(u0) + static_cast<std::size_t>(j)][0];
                        out = anti ? 0.5 * (value(2 * j) + value(2 * j + 1)) : value(j);
                    }
                }
                return w.block_samples.data();
            };
            auto unit_cv = [&](uint64_t u) -> std::vector<CvSample>
            {
                Worker &w = me();
                fill(w, rep_first + u, stratum(u), 1.0);
                evaluate(w);
                CvSample base;
                read_controls(w, base);
                if (theta)
                    for (int c = 0; c < k; ++c)
                        base.v[1 + c] *= w.path_weight;
                for (std::size_t l = 0; l < n_labels; ++l)
                {
                    w.out_cv[l] = base;
                    w.out_cv[l].v[0] = weighted(w, w.pay[l]);
                }
                if (!anti)
                    return w.out_cv;
                mirror(w, u);
                evaluate(w);
                CvSample other;
                read_controls(w, other);
                if (theta)
                    for (int c = 0; c < k; ++c)
                        other.v[1 + c] *= w.path_weight;
                for (std::size_t l = 0; l < n_labels; ++l)
                {
                    other.v[0] = weighted(w, w.pay[l]);
                    for (int i = 0; i <= k; ++i)
                        w.out_cv[l].v[i] = 0.5 * (w.out_cv[l].v[i] + other.v[i]);
                }
                return w.out_cv;
            };

            std::string note = "SimulationMCEngine + Philox";
            if (!stratified)
            {
                if (cv)
                    finish_cv(mc::reduce_logical_blocks<detail::ControlStats>(n_units, unit_cv, task_pool).acc);
                else
                {
                    const detail::LabelStats stats =
                        batch > 0 ? mc::reduce_logical_blocks_filled<detail::LabelStats>(n_units, fill_block, task_pool)
                                  : mc::reduce_logical_blocks<detail::LabelStats>(n_units, unit, task_pool);
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
                        const auto st =
                            mc::reduce_logical_blocks<detail::StratControlStats>(per_rep, unit_cv, task_pool);
                        for (std::size_t l = 0; l < n_labels && l < st.acc.size(); ++l)
                            reps[l].add(st.acc[l].estimate(controls.mean, k));
                    }
                    else
                    {
                        const auto st =
                            batch > 0 ? mc::reduce_logical_blocks_filled<detail::LabelStats>(per_rep, fill_block, task_pool)
                                      : mc::reduce_logical_blocks<detail::LabelStats>(per_rep, unit, task_pool);
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
                cv_note = cv ? cv_label() + " (within-stratum slope)" : "";
            }
            res.diagnostics = note + (anti ? " + antithetic" : "") + cv_note + cv_off + threads_note();
            return res;
        }

        // ── Pseudo-random: one PCG32 stream, on the calling thread ────────
        Worker &w = workers[0];
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
                    w.gauss[d] = -mirror[d];
            }
            else
            {
                for (std::size_t d = 0; d < dim; ++d)
                    w.gauss[d] = inv_normal ? inverse_normal_cdf(uniform01(rng))
                                            : bm(rng);
                if (antithetic)
                    std::copy_n(w.gauss.data(), dim, mirror.data());
            }
            if (cv)
                run_payoff_cv(w, acc_cv);
            else
                run_payoff(w, acc);
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

    namespace detail
    {
        /// The importance-sampling pilot: pseudo-random Philox, no control,
        /// on its own seed, one eighth of the run (4 096 to 65 536 paths).
        inline PricingSettings importance_pilot(const PricingSettings &s)
        {
            PricingSettings p = s;
            const int requested = s.mc_paths > 0 ? s.mc_paths : 100000;
            p.mc_paths = std::clamp(requested / 8, 4096, 65536);
            p.mc_seed = (s.mc_seed > 0 ? s.mc_seed : 1) ^ 0x5bd1e995;
            p.mc_sampler = SamplerKind::PseudoRandom;
            p.mc_rng = RngKind::Philox;
            p.mc_spot_control = false;
            return p;
        }
    } // namespace detail

    /**
     * @brief The one Monte-Carlo engine: simulate_impl, with generic
     *        importance sampling when settings.mc_importance_drift asks for it
     *        (blueprint/wp/19-gpu.md §2.5, engines/mc/importance_sampling.hpp).
     *
     * The GHS drift is a heuristic: a pilot runs with and without it, and the
     * run keeps it only if the pilot's variance falls -- otherwise it prices
     * without and says why in the diagnostics.
     */
    template <class T = Real>
    SimulationMCResult simulate(const ISimulatableProduct<T> &product, ISimulationModel<T> &model,
                                const PricingSettings &settings, ThreadPool *pool = nullptr)
    {
        if (!settings.mc_importance_drift)
            return simulate_impl<T>(product, model, settings, nullptr, pool);

        model.init(product.timeline(), product.defline());
        const mc::ImportanceDrift drift = mc::find_importance_drift(product, model);
        if (drift.theta.empty())
        {
            SimulationMCResult res = simulate_impl<T>(product, model, settings, nullptr, pool);
            res.diagnostics += " (no importance sampling: " + drift.why_none + ")";
            return res;
        }
        const PricingSettings pilot = detail::importance_pilot(settings);
        const Real plain_se = simulate_impl<T>(product, model, pilot, nullptr, pool).std_error();
        const Real is_se = simulate_impl<T>(product, model, pilot, &drift.theta, pool).std_error();
        if (!mc::importance_sampling_pays(plain_se, is_se))
        {
            SimulationMCResult res = simulate_impl<T>(product, model, settings, nullptr, pool);
            res.diagnostics += " (no importance sampling: the drift did not lower the pilot's variance)";
            return res;
        }
        SimulationMCResult res = simulate_impl<T>(product, model, settings, &drift.theta, pool);
        res.diagnostics += mc::importance_note(drift, plain_se, is_se);
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_HPP
