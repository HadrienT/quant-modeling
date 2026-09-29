#ifndef QM_ENGINES_MC_SIMULATION_ENGINE_AAD2_HPP
#define QM_ENGINES_MC_SIMULATION_ENGINE_AAD2_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tangent.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    using SecondOrderNumber = aad::Tangent<aad::Number>;

    /// One row of the Hessian of a Monte-Carlo price, with its errors.
    struct AADSecondOrderResults
    {
        Real price = 0.0;
        Real price_std_error = 0.0;
        std::string direction; ///< the parameter differentiated twice
        Real first = 0.0;      ///< d price / d direction (pathwise)
        Real first_std_error = 0.0;
        std::vector<std::string> risk_labels;
        std::vector<Real> second; ///< d² price / d direction d parameter_j
        std::vector<Real> second_std_errors;
        long long n_paths = 0;
        std::string diagnostics;
    };

    /**
     * @brief Second-order Monte-Carlo sensitivities by adjoint over tangent
     *        (blueprint/wp/17-aad.md §18): the price, its pathwise derivative
     *        in the direction of parameter `direction`, and the gradient of
     *        that derivative with respect to every parameter -- the Hessian
     *        row (gamma and vanna for direction = spot, volga for the vol).
     *
     * The model and the product run on Tangent<Number>: each parameter's
     * value is a tape leaf, its tangent is 1 for `direction` and 0 otherwise,
     * so a path's payoff carries its pathwise derivative d as a Number on the
     * tape. Propagating the adjoint from d (not from the payoff) back to the
     * leaves gives d²P / d direction d θ_j for every j at once. The tape
     * schedule is simulate_aad's (§7): parameters as leaves, init() recorded
     * once, a mark, per path rewind / generate / propagate to the mark, the
     * mark propagated once per batch of 64 for the standard errors (ADR-A6).
     *
     * The second derivative of a payoff with a hard kink is zero almost
     * everywhere: price digitals, barriers and vanillas with the fuzzy
     * evaluator for a meaningful gamma (the smoothing of lot 16c).
     */
    inline AADSecondOrderResults simulate_aad_second_order(const ISimulatableProduct<SecondOrderNumber> &product,
                                                           ISimulationModel<SecondOrderNumber> &model,
                                                           std::size_t direction, std::size_t n_paths,
                                                           std::uint64_t seed = 1)
    {
        using aad::Number;
        using aad::Tape;

        if (n_paths == 0)
            throw InvalidInput("simulate_aad_second_order: need at least one path");
        const std::vector<SecondOrderNumber *> &params = model.parameters();
        if (direction >= params.size())
            throw InvalidInput("simulate_aad_second_order: the direction is not a parameter of the model");

        Tape &tape = *Number::tape;
        tape.rewind();
        const detail::TapeClearGuard clear_on_exit{tape};

        // Leaves: every parameter's value; its tangent, the direction's unit vector.
        for (std::size_t j = 0; j < params.size(); ++j)
        {
            params[j]->v.put_on_tape();
            params[j]->d = Number(j == direction ? 1.0 : 0.0);
        }
        model.init(product.timeline(), product.defline());

        const std::size_t dim = model.sim_dim();
        Scenario<SecondOrderNumber> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<SecondOrderNumber> payoffs(product.payoff_labels().size());
        std::vector<double> gauss(std::max<std::size_t>(dim, 1));

        tape.mark();

        constexpr std::size_t BATCH = 64;
        WelfordAccumulator price_acc, first_acc;
        std::vector<WelfordAccumulator> second_acc(params.size());
        std::size_t done = 0;
        while (done < n_paths)
        {
            const std::size_t batch = std::min(BATCH, n_paths - done);
            WelfordAccumulator batch_price, batch_first;
            for (std::size_t p = 0; p < batch; ++p)
            {
                tape.rewind_to_mark();
                const auto index = static_cast<std::uint64_t>(done + p);
                for (std::size_t d = 0; d < dim; ++d)
                    gauss[d] = inverse_normal_cdf(philox_uniform(seed, index, static_cast<std::uint32_t>(d)));
                model.generate_path(std::span<const double>(gauss.data(), dim), path);
                product.payoffs(path, payoffs);
                SecondOrderNumber &result = payoffs.front();
                batch_price.add(result.v.value());
                batch_first.add(result.d.value());
                result.d.propagate_to_mark();
            }
            Number::propagate_mark_to_start();
            for (std::size_t j = 0; j < params.size(); ++j)
                second_acc[j].add(params[j]->v.adjoint() / static_cast<Real>(batch));
            tape.reset_adjoints_before_mark();
            price_acc.add(batch_price.mean);
            first_acc.add(batch_first.mean);
            done += batch;
        }

        AADSecondOrderResults res;
        res.price = price_acc.mean;
        res.price_std_error = price_acc.std_error();
        res.risk_labels = model.parameter_labels();
        res.direction = res.risk_labels.at(direction);
        res.first = first_acc.mean;
        res.first_std_error = first_acc.std_error();
        for (const WelfordAccumulator &a : second_acc)
        {
            res.second.push_back(a.mean);
            res.second_std_errors.push_back(a.std_error());
        }
        res.n_paths = static_cast<long long>(n_paths);
        res.diagnostics = "simulate_aad_second_order (adjoint over tangent, d/d" + res.direction +
                          ", batches of " + std::to_string(BATCH) + ") + Philox";
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_AAD2_HPP
