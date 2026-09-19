#ifndef QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
#define QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief The model a script is priced under -- chosen by the caller, since
     *        a script only describes a payoff and says nothing about the
     *        dynamics its price depends on (see script_analyzer.hpp /
     *        model_advice.hpp for how a mismatch is reported).
     *
     *  "black_scholes": one flat vol.
     *  "local_vol":     a Dupire surface in the shape calibrate_vol_surface
     *                   returns (K_grid, T_grid, sigma_loc_flat, K-major),
     *                   simulated by Euler steps of at most `max_dt`.
     */
    template <class T>
    std::unique_ptr<ISimulationModel<T>>
    make_script_model(const std::string &model, double spot, double rate,
                      double dividend, double vol,
                      const std::vector<double> &K_grid,
                      const std::vector<double> &T_grid,
                      const std::vector<double> &sigma_loc_flat, double max_dt)
    {
        if (model == "black_scholes")
        {
            if (!(vol > 0.0))
                throw InvalidInput("price_script: black_scholes needs vol > 0");
            return std::make_unique<BlackScholesSimModel<T>>(
                T(spot), T(rate), T(dividend), T(vol));
        }
        if (model == "local_vol")
        {
            std::vector<T> sigma;
            sigma.reserve(sigma_loc_flat.size());
            for (double x : sigma_loc_flat)
                sigma.emplace_back(x);
            return std::make_unique<LocalVolSimModel<T>>(
                T(spot), T(rate), T(dividend), K_grid, T_grid, std::move(sigma),
                max_dt);
        }
        throw std::invalid_argument(
            "price_script: unknown model '" + model +
            "' (expected 'black_scholes' or 'local_vol')");
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
