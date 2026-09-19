#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_PARALLEL_AAD_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_PARALLEL_AAD_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tape.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/rng_interface.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace quantModeling
{

    /**
     * @brief simulate_aad(), spread over a ThreadPool, with the same price
     *        and (§7.4-batched) risks as the series run regardless of how
     *        many worker threads are available (blueprint/wp/17-aad.md §8.3).
     *
     * The book's algorithm, adapted to this repo's per-batch risk variant
     * (already how simulate_aad() itself works, ADR-A6):
     *
     * Thread 0 is the *calling* thread: it reuses the caller's own Number
     * tape and the `model` passed in directly, exactly like simulate_aad().
     * Every other thread number 1..pool.num_threads() gets its own Tape and
     * its own clone() of `model` -- its parameters are then fresh leaves on
     * *that* tape, never the original's (the parameter-pointer trap every
     * model's copy constructor already guards against, ISimulationModel's
     * own doc comment). A worker's tape/model/rng are set up once and reused
     * across every batch it happens to process; which worker processes which
     * batch is decided by the pool, not by this function.
     *
     * Reproducibility (§8.4): every path always draws the exact same
     * gaussians regardless of which thread computes it, via
     * `rng.clone()` per thread and `RNG::skip_to(first_path_of_batch)` --
     * next_g() only, never Box-Muller, whose one-draw-in-reserve state would
     * desync under a jump-ahead. And unlike accumulating each batch's result
     * as it *arrives* (order depends on scheduling, and floating-point sums
     * are not associative), every batch's own price mean and risk vector is
     * stored by *batch index* and only reduced into the final
     * WelfordAccumulators afterwards, strictly in batch order -- bit-for-bit
     * identical to simulate_aad() run on 1 thread, and to itself on any
     * other thread count.
     *
     * Precondition carried over from the book (§6.1): `product.payoffs()`
     * must be safe to call concurrently from multiple threads -- true of
     * every hand-written ISimulatableProduct in this codebase (they hold no
     * mutable state), but **not yet true of ScriptedProduct<Number>**, whose
     * single shared Evaluator is not thread-safe (issue #12 tracks fixing
     * this separately; scripted products should keep using simulate_aad()
     * until it is).
     */
    inline AADSimulResults simulate_parallel_aad(
        const ISimulatableProduct<aad::Number> &product,
        ISimulationModel<aad::Number> &model, ThreadPool &pool, RNG &rng,
        std::size_t n_paths,
        const std::function<aad::Number(const std::vector<aad::Number> &)> &agg =
            first_aad_payoff)
    {
        using aad::Number;
        using aad::Tape;

        if (n_paths == 0)
            throw InvalidInput("simulate_parallel_aad: need at least one path");

        Tape &main_tape = *Number::tape;
        const detail::TapeClearGuard clear_on_exit{main_tape};

        constexpr std::size_t BATCH = 64;
        const std::size_t n_workers = pool.num_threads();
        const std::size_t n_batches = (n_paths + BATCH - 1) / BATCH;

        struct ThreadState
        {
            std::unique_ptr<Tape> tape;                      // null for thread 0
            std::unique_ptr<ISimulationModel<Number>> clone; // null for thread 0
            std::unique_ptr<RNG> rng;
            bool initialized = false;
            Scenario<Number> path;
            std::vector<Number> payoffs;
            std::vector<double> gauss;
            std::size_t num_params = 0;
        };
        std::vector<ThreadState> states(n_workers + 1);
        for (std::size_t i = 0; i <= n_workers; ++i)
            states[i].rng = rng.clone();
        for (std::size_t i = 1; i <= n_workers; ++i)
        {
            states[i].tape = std::make_unique<Tape>();
            states[i].clone = model.clone();
        }

        struct BatchResult
        {
            double price_mean = 0.0;
            std::vector<double> risk_means;
        };
        std::vector<BatchResult> batch_results(n_batches);

        auto run_batch = [&](std::size_t batch_index, std::size_t first_path,
                             std::size_t batch_size)
        {
            const std::size_t n = ThreadPool::thread_num();
            ThreadState &st = states[n];
            ISimulationModel<Number> &m = (n == 0) ? model : *st.clone;

            if (n > 0)
                Number::tape = st.tape.get();
            Tape &tape = *Number::tape;

            if (!st.initialized)
            {
                tape.rewind();
                m.put_parameters_on_tape();
                m.init(product.timeline(), product.defline());
                tape.mark();
                allocate_scenario(st.path, product.defline(), m.n_underlyings());
                st.payoffs.resize(product.payoff_labels().size());
                st.gauss.resize(std::max<std::size_t>(m.sim_dim(), 1));
                st.num_params = m.num_params();
                st.rng->init(m.sim_dim()); // must precede skip_to: Pcg32RNG
                                           // scales its jump by sim_dim()
                st.initialized = true;
            }

            const std::size_t dim = m.sim_dim();
            st.rng->skip_to(first_path);

            double price_sum = 0.0;
            for (std::size_t p = 0; p < batch_size; ++p)
            {
                tape.rewind_to_mark();
                st.rng->next_g(std::span<double>(st.gauss.data(), dim));
                m.generate_path(std::span<const double>(st.gauss.data(), dim),
                                st.path);
                product.payoffs(st.path, st.payoffs);
                Number result = agg(st.payoffs);
                price_sum += result.value();
                result.propagate_to_mark();
            }

            BatchResult &br = batch_results[batch_index];
            br.price_mean = price_sum / static_cast<double>(batch_size);
            br.risk_means.assign(st.num_params, 0.0);
            if (st.num_params > 0)
            {
                Number::propagate_mark_to_start();
                for (std::size_t j = 0; j < st.num_params; ++j)
                    br.risk_means[j] = m.parameters()[j]->adjoint() /
                                       static_cast<Real>(batch_size);
                tape.reset_adjoints_before_mark();
            }
            return true;
        };

        std::vector<TaskHandle> handles;
        handles.reserve(n_batches);
        for (std::size_t b = 0; b < n_batches; ++b)
        {
            const std::size_t first = b * BATCH;
            const std::size_t size = std::min(BATCH, n_paths - first);
            handles.push_back(pool.spawn_task(
                [&run_batch, b, first, size]
                { return run_batch(b, first, size); }));
        }
        for (TaskHandle &h : handles)
            pool.active_wait(h);

        const std::size_t n_params = model.num_params();
        WelfordAccumulator price_acc;
        std::vector<WelfordAccumulator> risk_acc(n_params);
        for (std::size_t b = 0; b < n_batches; ++b)
        {
            price_acc.add(batch_results[b].price_mean);
            for (std::size_t j = 0; j < n_params; ++j)
                risk_acc[j].add(batch_results[b].risk_means[j]);
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
        res.n_paths = static_cast<long long>(n_paths);
        res.diagnostics = "simulate_parallel_aad (" + std::to_string(n_workers) +
                          " worker thread(s), batches of " +
                          std::to_string(BATCH) + ")";
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_PARALLEL_AAD_HPP
