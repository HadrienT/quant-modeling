#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP

#include "quantModeling/aad/number.hpp"
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
        tape.rewind(); // 1. empty tape, memory kept

        model.put_parameters_on_tape();                     // 2. leaves: the parameters
        model.init(product.timeline(), product.defline());  // 3. precomputations, RECORDED

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

        tape.clear();
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_AAD_HPP
