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

#include <cmath>
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
     *        (blueprint/wp/17-aad.md §12.2, ADR-A10): the price, its pathwise derivative
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
     * Exact only when the payoff is C¹ in the parameters along every path.
     * A hard kink has a zero second derivative almost everywhere, and so
     * does the fuzzy evaluator's call spread (lot 16c): it smooths the
     * payoff but not its derivative, whose jumps at ±eps are Dirac masses
     * the pathwise derivative never sees. A fuzzy call's pathwise gamma
     * tends to twice the true gamma. Digitals, barriers and vanillas take
     * simulate_aad_bumped_second_order, the book's method (§12.1).
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

    /**
     * @brief The book's second order (blueprint/wp/17-aad.md §12.1): a
     *        Hessian row by central differences on adjoint risks.
     *
     * Bumps parameter `direction` by ±h, h = rel_bump · |θ| (rel_bump when
     * θ = 0), and runs simulate_aad on each side with the same Philox seed:
     * second[j] = (∂V/∂θ_j(θ + h) − ∂V/∂θ_j(θ − h)) / 2h, a whole row for two
     * adjoint runs. A third, unbumped run gives the price and the first
     * derivative. The random numbers are common and the batches aligned, so
     * each second derivative's standard error is that of the batch-wise
     * differences -- the correlation between the two sides included.
     *
     * The delta of a fuzzy (call-spread) payoff is continuous in the
     * parameters, so the difference converges to the smoothed product's
     * gamma, with an O(h²) bias; where adjoint over tangent is exact (C¹
     * payoffs) the two agree.
     */
    inline AADSecondOrderResults simulate_aad_bumped_second_order(const ISimulatableProduct<aad::Number> &product,
                                                                  ISimulationModel<aad::Number> &model,
                                                                  std::size_t direction, std::size_t n_paths,
                                                                  std::uint64_t seed = 1, double rel_bump = 1e-2)
    {
        const std::vector<aad::Number *> &params = model.parameters();
        if (direction >= params.size())
            throw InvalidInput("simulate_aad_bumped_second_order: the direction is not a parameter of the model");
        if (!(rel_bump > 0.0))
            throw InvalidInput("simulate_aad_bumped_second_order: rel_bump must be > 0");

        double &theta = params[direction]->value();
        const double theta0 = theta;
        const double h = theta0 != 0.0 ? rel_bump * std::abs(theta0) : rel_bump;
        const auto run = [&](double value, std::vector<std::vector<Real>> *batches)
        {
            theta = value;
            return simulate_aad(product, model, n_paths, seed, first_aad_payoff, RngKind::Philox,
                                SamplerKind::PseudoRandom, batches);
        };

        std::vector<std::vector<Real>> up_batches, down_batches;
        AADSimulResults up, down, centre;
        try
        {
            up = run(theta0 + h, &up_batches);
            down = run(theta0 - h, &down_batches);
            centre = run(theta0, nullptr);
        }
        catch (...)
        {
            theta = theta0;
            throw;
        }
        theta = theta0;

        AADSecondOrderResults res;
        res.price = centre.price;
        res.price_std_error = centre.price_std_error;
        res.risk_labels = centre.risk_labels;
        res.direction = res.risk_labels.at(direction);
        res.first = centre.risks[direction];
        res.first_std_error = centre.risk_std_errors[direction];
        std::vector<WelfordAccumulator> second_acc(params.size());
        for (std::size_t b = 0; b < up_batches.size(); ++b)
            for (std::size_t j = 0; j < params.size(); ++j)
                second_acc[j].add((up_batches[b][j] - down_batches[b][j]) / (2.0 * h));
        for (const WelfordAccumulator &a : second_acc)
        {
            res.second.push_back(a.mean);
            res.second_std_errors.push_back(a.std_error());
        }
        res.n_paths = centre.n_paths;
        res.diagnostics = "simulate_aad_bumped_second_order (central difference of adjoint risks, d/d" +
                          res.direction + ", h = " + std::to_string(h) + ") + Philox";
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_SIMULATION_ENGINE_AAD2_HPP
