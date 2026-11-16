#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/results.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <optional>
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /// Sobol RQMC replicates of an adjoint run (simulate_aad, and the GPU's
    /// dual and adjoint engines, engines/mc/script_engine.hpp).
    constexpr int kAadRqmcReplicates = 16;

    struct AADSimulResults
    {
        Real price = 0.0;
        Real price_std_error = 0.0;
        std::vector<std::string> risk_labels;
        std::vector<Real> risks;
        std::vector<Real> risk_std_errors; ///< blueprint §7.4 — one per risk, like every other MC number the project reports
        long long n_paths = 0;
        std::string diagnostics;
        int gpus = 0; ///< GPUs that computed the risks (0: the CPU tape)
        /// A multilevel run's estimate of its remaining discretisation
        /// bias on the price (WP 26 §10.2); unset for a plain run.
        std::optional<Real> discretisation_bias;
        /// Sobol RQMC on the GPU only: each replicate's risks (replicates x
        /// risks, replicate-major). Two runs on the same tables are paired
        /// replicate by replicate (the bump on the adjoint, WP 27 §14.10).
        std::vector<Real> replicate_risks;
    };

    /// Default aggregator: the product's first (and, for every product in
    /// this lot, only) payoff.
    inline aad::Number first_aad_payoff(const std::vector<aad::Number> &payoffs)
    {
        return payoffs.front();
    }

    namespace detail
    {
        /// Guarantees tape.clear() runs on every way out of simulate_aad --
        /// including an exception thrown mid-run (a model bug, an
        /// unexpectedly large product). Without this, a run that throws
        /// leaves whatever it had recorded up to that point parked on the
        /// calling thread's tape indefinitely: not unbounded (still capped
        /// by that one run's own ceiling), but wasted until some later,
        /// successful run on the same thread happens to clear it.
        struct TapeClearGuard
        {
            aad::Tape &tape;
            ~TapeClearGuard() { tape.clear(); }
        };

        /// Turns on Tape::multi / sets Node::num_adj for the duration of
        /// simulate_aad_multi, restoring both on every way out. Both are
        /// plain statics, not thread_local (unlike Number::tape itself) --
        /// lot 17f is single-threaded by design, and combining it with
        /// simulate_parallel_aad (lot 17d) is out of scope: every thread
        /// would record in the same mode at once. The guard's job here is
        /// narrower, but still necessary -- a run that throws, or simply
        /// returns, must not leave the flag on for whatever mono-adjoint
        /// simulate_aad call happens next on this thread.
        struct MultiModeGuard
        {
            std::size_t prev_num_adj = aad::Node::num_adj;
            bool prev_multi = aad::Tape::is_multi();
            explicit MultiModeGuard(std::size_t m)
            {
                aad::Node::num_adj = m;
                aad::Tape::set_multi(true);
            }
            ~MultiModeGuard()
            {
                aad::Node::num_adj = prev_num_adj;
                aad::Tape::set_multi(prev_multi);
            }
        };
    } // namespace detail

    /**
     * @brief Adjoint Monte-Carlo: the price and every model-parameter risk in
     *        one run, at O(1) extra cost in the number of parameters.
     *
     * blueprint/wp/17-aad.md §7, step by step:
     *  1. rewind the tape (memory kept);
     *  2. the model's parameters become fresh leaves;
     *  3. init() runs *once*, recorded -- its precomputations (e.g. a drift)
     *     are on the tape exactly once, not once per path;
     *  4. mark() -- the boundary between "recorded once" and "recorded per
     *     path";
     *  5-6. each path: rewind_to_mark() forgets the previous path, generate
     *     it, aggregate its payoff(s) to one scalar, propagate its adjoint
     *     back *to the mark only*;
     *  7. once per batch, propagate the mark back to the parameters and read
     *     their adjoints -- correct by linearity (§7.2): the nodes before the
     *     mark are never recreated, so their adjoints accumulate the sum of
     *     every path's contribution in the batch, and propagating that sum
     *     once is the same as propagating each path's contribution and
     *     summing the results.
     *
     * Departs from the book in one place (ADR-A6): the book propagates the
     * mark to the parameters exactly once, at the very end. Here it happens
     * once every 64 paths instead, with the pre-mark adjoints reset between
     * batches, so that every risk gets a standard error the same way the
     * project already reports one for every other Monte-Carlo number (the
     * batch means are i.i.d., same scheme as the Sobol RQMC batches in
     * simulate()) -- at the cost of one extra propagation of the
     * precomputation section per batch, negligible next to 64 path
     * propagations.
     *
     * `batch_risks`, when given, receives each batch's risks (one row per
     * batch): two runs on the same seed and path count have the same
     * batches, so a difference of their risks gets its standard error from
     * the batch-wise differences (simulate_aad_bumped_second_order, §12).
     *
     * `pool`, when given with worker threads, runs the batches in parallel
     * (WP 26) -- Sobol replicates or Philox batches of 64, each thread on its
     * own tape with its own clone of the model (parameters as leaves on that
     * tape, precomputations recorded once before its own mark) and its own
     * payoff state; batches are folded in batch order, so the price, the
     * risks and `batch_risks` have the same bits on any number of threads.
     * PCG32 (one stream drawn in path order), a product without a
     * concurrent payoff state (early exercise) and the multi-adjoint mode
     * stay on the calling thread. Measured 2026-11-16, rough Bergomi on the
     * daily grid, 200 000 Sobol paths of a one-year call: 26.0 s on one
     * thread, 13.2 s on two, 1.75 s for 50 000 on four.
     */
    inline AADSimulResults simulate_aad(
        const ISimulatableProduct<aad::Number> &product,
        ISimulationModel<aad::Number> &model, std::size_t n_paths,
        std::uint64_t seed = 1,
        const std::function<aad::Number(const std::vector<aad::Number> &)> &agg =
            first_aad_payoff,
        RngKind rng_kind = RngKind::Pcg32, SamplerKind sampler = SamplerKind::PseudoRandom,
        std::vector<std::vector<Real>> *batch_risks = nullptr, ThreadPool *pool = nullptr)
    {
        using aad::Number;
        using aad::Tape;

        if (n_paths == 0)
            throw InvalidInput("simulate_aad: need at least one path");
        if (batch_risks)
            batch_risks->clear();
        // Philox: path p's draw j is Phi^-1(Philox(seed, p, j)), as on the GPU
        // (blueprint/wp/19-gpu.md §6) -- the oracle of its dual and adjoint runs.
        const bool philox = rng_kind == RngKind::Philox;

        Tape &tape = *Number::tape;
        tape.rewind();                                    // 1. empty tape, memory kept
        const detail::TapeClearGuard clear_on_exit{tape}; // runs on every exit, exception included

        model.put_parameters_on_tape();                    // 2. leaves: the parameters
        model.init(product.timeline(), product.defline()); // 3. precomputations, RECORDED

        const std::size_t dim = model.sim_dim();
        const std::size_t n_params = model.num_params();

        // Sobol RQMC (blueprint/wp/19-gpu.md §2.2, §2.4): kAadRqmcReplicates
        // digitally shifted replicates, the generic engine's scheme and
        // Brownian bridge; a batch is a replicate, so the risks' error is
        // the replicates' spread.
        const bool sobol = sampler == SamplerKind::Sobol && dim > 0;
        const std::size_t per_rep =
            sobol ? sobol_points_per_replicate(n_paths, static_cast<std::size_t>(kAadRqmcReplicates)) : 0;
        const std::size_t total = sobol ? per_rep * static_cast<std::size_t>(kAadRqmcReplicates) : n_paths;
        const BrownianLayout layout = sobol ? model.brownian_layout() : BrownianLayout{};
        const bool bridged = sobol && layout.covers(dim);

        tape.mark(); // 4. THE MARK

        constexpr std::size_t BATCH = 64;
        const std::size_t batch_paths = sobol ? per_rep : BATCH;
        const std::size_t n_batches = (total + batch_paths - 1) / batch_paths;

        /// What a thread needs to run batches: its tape (null: the calling
        /// thread's), its model (a clone whose parameters are leaves on that
        /// tape, recorded once with its precomputations before its own
        /// mark), the product's per-thread state, its buffers.
        struct Worker
        {
            std::unique_ptr<Tape> own_tape;
            std::unique_ptr<ISimulationModel<Number>> clone;
            ISimulationModel<Number> *model = nullptr;
            std::unique_ptr<typename ISimulatableProduct<Number>::PayoffState> state;
            bool ready = false;
            Scenario<Number> path;
            std::vector<Number> payoffs;
            std::vector<double> gauss, point;
            std::optional<BridgedGaussians> bridge;
        };
        /// A batch's price mean and risks, folded in batch order afterwards.
        struct BatchResult
        {
            Real price_mean = 0.0;
            std::vector<Real> risks;
        };
        std::vector<BatchResult> results(n_batches);

        const auto prepare = [&](Worker &w)
        {
            allocate_scenario(w.path, product.defline(), w.model->n_underlyings());
            w.payoffs.resize(product.payoff_labels().size());
            w.gauss.assign(std::max<std::size_t>(dim, 1), 0.0);
            if (bridged)
            {
                w.bridge.emplace(layout.times, layout.factors, layout.stride);
                w.point.assign(dim, 0.0);
            }
            w.ready = true;
        };

        // 5-7 for one batch on the calling thread's tape, whose mark is
        // already set. A batch's numbers depend on its paths only: the
        // pre-mark adjoints start from zero (reset after every batch), so
        // the same batch gives the same bits on any thread.
        Pcg32 rng = RngFactory(seed).make(0);
        NormalBoxMuller bm;
        const auto run_batch = [&](Worker &w, std::size_t b)
        {
            Tape &t = *Number::tape;
            ISimulationModel<Number> &m = *w.model;
            const std::size_t done = b * batch_paths;
            const std::size_t batch_size = std::min(batch_paths, total - done);
            std::optional<SobolSequence> sequence;
            if (sobol)
                sequence.emplace(static_cast<int>(dim),
                                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)) << 32) |
                                     static_cast<std::uint64_t>(b));
            WelfordAccumulator batch_price;
            for (std::size_t p = 0; p < batch_size; ++p)
            {
                t.rewind_to_mark(); // 5. forget the previous path
                const auto path_index = static_cast<std::uint64_t>(done + p);
                if (sobol && w.bridge)
                {
                    sequence->next_gaussian(std::span<double>(w.point.data(), dim));
                    w.bridge->map(w.point, std::span<double>(w.gauss.data(), dim));
                }
                else if (sobol)
                    sequence->next_gaussian(std::span<double>(w.gauss.data(), dim));
                else
                    for (std::size_t d = 0; d < dim; ++d)
                        w.gauss[d] = philox ? inverse_normal_cdf(
                                                  philox_uniform(seed, path_index, static_cast<std::uint32_t>(d)))
                                            : bm(rng);
                m.generate_path(std::span<const double>(w.gauss.data(), dim), w.path); // recorded after the mark
                if (w.state)
                    product.payoffs_with_state(w.path, w.payoffs, *w.state);
                else
                    product.payoffs(w.path, w.payoffs);
                Number result = agg(w.payoffs);
                batch_price.add(result.value());
                result.propagate_to_mark(); // 6. this path's adjoint -> up to the mark
            }
            BatchResult &r = results[b];
            r.price_mean = batch_price.mean;
            r.risks.assign(n_params, 0.0);
            if (n_params > 0)
            {
                Number::propagate_mark_to_start(); // 7. mark -> parameters, once per batch
                for (std::size_t j = 0; j < n_params; ++j)
                    r.risks[j] = m.parameters()[j]->adjoint() / static_cast<Real>(batch_size);
                t.reset_adjoints_before_mark(); // next batch starts from zero
            }
        };

        // Several threads (WP 26): the batches are independent once each
        // thread has its own tape, model clone and payoff state -- not for
        // PCG32 (one stream, drawn in path order), nor a product whose
        // payoffs cannot run concurrently (early exercise), nor the
        // multi-adjoint mode (process-wide flags).
        std::unique_ptr<typename ISimulatableProduct<Number>::PayoffState> first_state;
        const bool parallel = pool != nullptr && pool->num_threads() > 0 && n_batches > 1 &&
                              (sobol || philox) && ThreadPool::thread_num() == 0 && !Tape::is_multi() &&
                              (first_state = product.make_payoff_state()) != nullptr;
        std::vector<Worker> workers(parallel ? pool->num_threads() + 1 : 1);
        workers[0].model = &model;
        workers[0].state = std::move(first_state);
        prepare(workers[0]);
        if (!parallel)
        {
            for (std::size_t b = 0; b < n_batches; ++b)
                run_batch(workers[0], b);
        }
        else
        {
            // The clones are made here, before any batch runs on `model`;
            // each thread then puts its clone's parameters on its own tape.
            for (std::size_t i = 1; i < workers.size(); ++i)
            {
                workers[i].own_tape = std::make_unique<Tape>();
                workers[i].clone = model.clone();
                workers[i].model = workers[i].clone.get();
            }
            std::vector<TaskHandle> handles;
            handles.reserve(n_batches);
            for (std::size_t b = 0; b < n_batches; ++b)
                handles.push_back(pool->spawn_task(
                    [&, b]
                    {
                        Worker &w = workers[ThreadPool::thread_num()];
                        if (w.own_tape)
                        {
                            Tape *const before = Number::tape;
                            Number::tape = w.own_tape.get();
                            try
                            {
                                if (!w.ready)
                                {
                                    // This thread's 1-4: rewind, leaves,
                                    // precomputations, mark.
                                    w.own_tape->rewind();
                                    w.model->put_parameters_on_tape();
                                    w.model->init(product.timeline(), product.defline());
                                    w.own_tape->mark();
                                    w.state = product.make_payoff_state();
                                    prepare(w);
                                }
                                run_batch(w, b);
                            }
                            catch (...)
                            {
                                Number::tape = before;
                                throw;
                            }
                            Number::tape = before;
                        }
                        else
                            run_batch(w, b);
                        return true;
                    }));
            // Every batch finishes before an error leaves: they reference this frame.
            pool->active_wait_all(handles);
        }

        WelfordAccumulator price_acc;
        std::vector<WelfordAccumulator> risk_acc(n_params);
        for (std::size_t b = 0; b < n_batches; ++b)
        {
            price_acc.add(results[b].price_mean);
            for (std::size_t j = 0; j < n_params; ++j)
                risk_acc[j].add(results[b].risks[j]);
            if (batch_risks && n_params > 0)
                batch_risks->push_back(results[b].risks);
        }

        AADSimulResults res;
        res.price = price_acc.mean;
        res.price_std_error = price_acc.std_error();
        res.risk_labels = model.parameter_labels();
        res.risks.resize(n_params);
        res.risk_std_errors.resize(n_params);
        for (std::size_t j = 0; j < n_params; ++j)
        {
            res.risks[j] = risk_acc[j].mean;
            res.risk_std_errors[j] = risk_acc[j].std_error();
        }
        res.n_paths = static_cast<long long>(total);
        if (sobol)
            res.diagnostics = "simulate_aad (adjoint) + Sobol RQMC (" + std::to_string(kAadRqmcReplicates) +
                              " batches)" + (bridged ? " + Brownian bridge" : "");
        else
            res.diagnostics = "simulate_aad (adjoint, batches of " + std::to_string(BATCH) + ")" +
                              (philox ? " + Philox" : "");
        if (parallel)
            res.diagnostics += " on " + std::to_string(workers.size()) + " threads";

        return res;
    }

    /// blueprint §9: every payoff's sensitivities to every model parameter,
    /// from one recorded path per Monte-Carlo path instead of one per
    /// payoff. `risks`/`risk_std_errors` are row-major, payoff-major:
    /// entry (payoff i, parameter j) is at `i * risk_labels.size() + j`.
    struct AADMultiSimulResults
    {
        std::vector<std::string> payoff_labels;
        std::vector<Real> prices;           ///< one per payoff
        std::vector<Real> price_std_errors; ///< one per payoff
        std::vector<std::string> risk_labels;
        std::vector<Real> risks;           ///< payoff_labels.size() * risk_labels.size()
        std::vector<Real> risk_std_errors; ///< same shape
        long long n_paths = 0;
        std::string diagnostics;
    };

    /**
     * @brief Adjoint Monte-Carlo for *every* payoff at once: a
     *        payoff_labels().size() x num_params() sensitivity matrix, for
     *        roughly the cost of one simulate_aad run regardless of how
     *        many payoffs there are (blueprint §9).
     *
     * Same structure as simulate_aad (§7: rewind, parameters as leaves,
     * init() recorded once, mark, then per-path rewind_to_mark / generate /
     * propagate-to-mark, batched risk propagation for a standard error on
     * every entry -- §7.4). The one difference is what gets seeded and how
     * it propagates: instead of aggregating every payoff into one scalar
     * and giving it adjoint 1, *each* payoff i is seeded on its own
     * component (`payoffs[i].adjoint(i) = 1`), and one backward pass with
     * Node::propagate_all() carries every payoff's adjoint through the path
     * simultaneously -- the vector-mode reverse pass §9 describes.
     */
    inline AADMultiSimulResults simulate_aad_multi(
        const ISimulatableProduct<aad::Number> &product,
        ISimulationModel<aad::Number> &model, std::size_t n_paths,
        std::uint64_t seed = 1)
    {
        using aad::Number;
        using aad::Tape;

        if (n_paths == 0)
            throw InvalidInput("simulate_aad_multi: need at least one path");

        const std::size_t m = product.payoff_labels().size();
        if (m == 0)
            throw InvalidInput("simulate_aad_multi: product has no payoffs");

        Tape &tape = *Number::tape;
        const detail::MultiModeGuard multi_guard(m); // Node::num_adj = m, Tape::multi = true
        tape.rewind();
        const detail::TapeClearGuard clear_on_exit{tape};

        model.put_parameters_on_tape();
        model.init(product.timeline(), product.defline());

        const std::size_t dim = model.sim_dim();
        const std::size_t n_params = model.num_params();

        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<Number> payoffs(m);
        std::vector<double> gauss(std::max<std::size_t>(dim, 1));

        Pcg32 rng = RngFactory(seed).make(0);
        NormalBoxMuller bm;

        tape.mark();

        constexpr std::size_t BATCH = 64;
        std::vector<WelfordAccumulator> price_acc(m);
        std::vector<WelfordAccumulator> risk_acc(m * n_params);

        std::size_t done = 0;
        while (done < n_paths)
        {
            const std::size_t batch_size = std::min(BATCH, n_paths - done);
            std::vector<WelfordAccumulator> batch_price(m);

            for (std::size_t p = 0; p < batch_size; ++p)
            {
                tape.rewind_to_mark();
                for (std::size_t d = 0; d < dim; ++d)
                    gauss[d] = bm(rng);
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, payoffs);
                for (std::size_t i = 0; i < m; ++i)
                {
                    batch_price[i].add(payoffs[i].value());
                    payoffs[i].adjoint(i) = 1.0; // seed this payoff's own component only
                }
                Number::propagate_to_mark_multi(); // one pass, every payoff at once
            }

            if (n_params > 0)
            {
                Number::propagate_mark_to_start_multi();
                for (std::size_t j = 0; j < n_params; ++j)
                    for (std::size_t i = 0; i < m; ++i)
                        risk_acc[i * n_params + j].add(
                            model.parameters()[j]->adjoint(i) / static_cast<Real>(batch_size));
                tape.reset_adjoints_before_mark(); // clears every payoff's row, not just one
            }

            for (std::size_t i = 0; i < m; ++i)
                price_acc[i].add(batch_price[i].mean);
            done += batch_size;
        }

        AADMultiSimulResults res;
        res.payoff_labels = product.payoff_labels();
        res.prices.resize(m);
        res.price_std_errors.resize(m);
        for (std::size_t i = 0; i < m; ++i)
        {
            res.prices[i] = price_acc[i].mean;
            res.price_std_errors[i] = price_acc[i].std_error();
        }
        res.risk_labels = model.parameter_labels();
        res.risks.resize(m * n_params);
        res.risk_std_errors.resize(m * n_params);
        for (std::size_t idx = 0; idx < m * n_params; ++idx)
        {
            res.risks[idx] = risk_acc[idx].mean;
            res.risk_std_errors[idx] = risk_acc[idx].std_error();
        }
        res.n_paths = static_cast<long long>(n_paths);
        res.diagnostics = "simulate_aad_multi (multi-adjoint, " + std::to_string(m) +
                          " payoffs, batches of " + std::to_string(BATCH) + ")";

        return res;
    }

    /**
     * @brief Wraps an adjoint run into the PricingResult every pricer
     *        already returns (blueprint §13.1).
     *
     * `risks` carries every parameter's sensitivity by name, whatever the
     * model -- a local-vol grid's 1500 points (lot 17e) will not fit in
     * Greeks' five named slots. For a model whose labels happen to match
     * the classic ones ("spot", "rate", "div", "vol" -- BlackScholesSimModel
     * today), the matching Greeks fields are filled too, so front-end code
     * built against Greeks keeps working unchanged. Theta and gamma are
     * left unset: time is not a model parameter (theta stays bump), and
     * second order is a separate run (simulate_aad_bumped_second_order, §12).
     */
    inline PricingResult to_pricing_result(const AADSimulResults &r)
    {
        PricingResult out;
        out.npv = r.price;
        out.mc_std_error = r.price_std_error;
        out.diagnostics = r.diagnostics;
        out.risks = RiskReport{r.risk_labels, r.risks, r.risk_std_errors};
        out.gpus = r.gpus;
        out.discretisation_bias = r.discretisation_bias;

        for (std::size_t i = 0; i < r.risk_labels.size(); ++i)
        {
            if (r.risk_labels[i] == "spot")
            {
                out.greeks.delta = r.risks[i];
                out.greeks.delta_std_error = r.risk_std_errors[i];
            }
            else if (r.risk_labels[i] == "vol")
            {
                out.greeks.vega = r.risks[i];
                out.greeks.vega_std_error = r.risk_std_errors[i];
            }
            else if (r.risk_labels[i] == "rate")
            {
                out.greeks.rho = r.risks[i];
                out.greeks.rho_std_error = r.risk_std_errors[i];
            }
        }
        return out;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP
