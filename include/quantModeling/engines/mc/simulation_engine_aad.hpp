#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/results.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    struct AADSimulResults
    {
        Real price = 0.0;
        Real price_std_error = 0.0;
        std::vector<std::string> risk_labels;
        std::vector<Real> risks;
        std::vector<Real> risk_std_errors; ///< blueprint §7.4 — one per risk, like every other MC number the project reports
        long long n_paths = 0;
        std::string diagnostics;
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
     */
    inline AADSimulResults simulate_aad(
        const ISimulatableProduct<aad::Number> &product,
        ISimulationModel<aad::Number> &model, std::size_t n_paths,
        std::uint64_t seed = 1,
        const std::function<aad::Number(const std::vector<aad::Number> &)> &agg =
            first_aad_payoff)
    {
        using aad::Number;
        using aad::Tape;

        if (n_paths == 0)
            throw InvalidInput("simulate_aad: need at least one path");

        Tape &tape = *Number::tape;
        tape.rewind();                                    // 1. empty tape, memory kept
        const detail::TapeClearGuard clear_on_exit{tape}; // runs on every exit, exception included

        model.put_parameters_on_tape();                    // 2. leaves: the parameters
        model.init(product.timeline(), product.defline()); // 3. precomputations, RECORDED

        const std::size_t dim = model.sim_dim();
        const std::size_t n_params = model.num_params();

        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<Number> payoffs(product.payoff_labels().size());
        std::vector<double> gauss(std::max<std::size_t>(dim, 1));

        Pcg32 rng = RngFactory(seed).make(0);
        NormalBoxMuller bm;

        tape.mark(); // 4. THE MARK

        constexpr std::size_t BATCH = 64;
        WelfordAccumulator price_acc;
        std::vector<WelfordAccumulator> risk_acc(n_params);

        std::size_t done = 0;
        while (done < n_paths)
        {
            const std::size_t batch_size = std::min(BATCH, n_paths - done);
            WelfordAccumulator batch_price;

            for (std::size_t p = 0; p < batch_size; ++p)
            {
                tape.rewind_to_mark(); // 5. forget the previous path
                for (std::size_t d = 0; d < dim; ++d)
                    gauss[d] = bm(rng);
                model.generate_path(std::span<const double>(gauss.data(), dim), path); // recorded after the mark
                product.payoffs(path, payoffs);
                Number result = agg(payoffs);
                batch_price.add(result.value());
                result.propagate_to_mark(); // 6. this path's adjoint -> up to the mark
            }

            if (n_params > 0)
            {
                Number::propagate_mark_to_start(); // 7. mark -> parameters, once per batch
                for (std::size_t j = 0; j < n_params; ++j)
                    risk_acc[j].add(model.parameters()[j]->adjoint() /
                                    static_cast<Real>(batch_size));
                tape.reset_adjoints_before_mark(); // next batch starts from zero
            }

            price_acc.add(batch_price.mean);
            done += batch_size;
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
        res.diagnostics =
            "simulate_aad (adjoint, batches of " + std::to_string(BATCH) + ")";

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
     * exact second order is out of scope for this lot (§12).
     */
    inline PricingResult to_pricing_result(const AADSimulResults &r)
    {
        PricingResult out;
        out.npv = r.price;
        out.mc_std_error = r.price_std_error;
        out.diagnostics = r.diagnostics;
        out.risks = RiskReport{r.risk_labels, r.risks, r.risk_std_errors};

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
