#ifndef QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
#define QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/heston.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quantModeling::scripting
{

    /// What make_script_model needs: the model's name and its calibrated
    /// inputs. Each model reads only its own fields.
    struct ScriptModelSpec
    {
        std::string model = "black_scholes";
        double spot = 0.0;
        double rate = 0.0;
        double dividend = 0.0;
        double vol = 0.0; ///< black_scholes
        /// local_vol: the Dupire grid; slv: the leverage grid's axes.
        std::vector<double> K_grid;
        std::vector<double> T_grid;
        std::vector<double> sigma_loc_flat; ///< local_vol, K-major
        HestonParams heston;                ///< heston, slv
        std::vector<double> leverage_flat;  ///< slv, K-major on (K_grid, T_grid)
        double max_dt = 1.0 / 52.0;         ///< Euler step bound (every model but black_scholes)
    };

    /**
     * @brief The model a script is priced under. A script only describes a
     *        payoff; which dynamics its price depends on is read off it by
     *        script_analyzer.hpp, and model_advice.hpp's recommend() turns
     *        that into a choice (the caller may override it).
     *
     *  "black_scholes": one flat vol, simulated exactly.
     *  "local_vol":     a Dupire surface in the shape calibrate_vol_surface
     *                   returns (K_grid, T_grid, sigma_loc_flat, K-major).
     *  "heston":        Heston stochastic volatility with calibrated
     *                   parameters (market/heston_calibration.hpp): the
     *                   Bates simulation model with no jumps, full-truncation
     *                   Euler.
     *  "slv":           stochastic-local volatility -- the calibrated Heston
     *                   dynamics times a leverage L(S, t) calibrated so that
     *                   the marginals are the Dupire surface's
     *                   (market/slv_calibration.hpp).
     */
    template <class T>
    std::unique_ptr<ISimulationModel<T>> make_script_model(const ScriptModelSpec &s)
    {
        auto to_T = [](const std::vector<double> &xs)
        {
            std::vector<T> out;
            out.reserve(xs.size());
            for (double x : xs)
                out.emplace_back(x);
            return out;
        };
        const HestonParams &h = s.heston;

        if (s.model == "black_scholes")
        {
            if (!(s.vol > 0.0))
                throw InvalidInput("price_script: black_scholes needs vol > 0");
            return std::make_unique<BlackScholesSimModel<T>>(
                T(s.spot), T(s.rate), T(s.dividend), T(s.vol));
        }
        if (s.model == "local_vol")
            return std::make_unique<LocalVolSimModel<T>>(
                T(s.spot), T(s.rate), T(s.dividend), s.K_grid, s.T_grid,
                to_T(s.sigma_loc_flat), s.max_dt);
        if (s.model == "heston")
            return std::make_unique<BatesSimModel<T>>(
                T(s.spot), T(s.rate), T(s.dividend), T(h.v0), T(h.kappa),
                T(h.theta), T(h.xi), T(h.rho), T(0.0), T(0.0), T(0.0), s.max_dt);
        if (s.model == "slv")
            return std::make_unique<SLVSimModel<T>>(
                T(s.spot), T(s.rate), T(s.dividend), T(h.v0), T(h.kappa),
                T(h.theta), T(h.xi), T(h.rho), s.K_grid, s.T_grid,
                to_T(s.leverage_flat), s.max_dt);
        throw std::invalid_argument(
            "price_script: unknown model '" + s.model +
            "' (expected 'black_scholes', 'local_vol', 'heston' or 'slv')");
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
