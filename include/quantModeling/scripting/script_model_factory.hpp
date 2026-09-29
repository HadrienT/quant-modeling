#ifndef QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
#define QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/hybrid/hull_white_equity_sim_model.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/heston.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_local_vol_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <Eigen/Core>

#include <cmath>
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
        /// black_scholes on several underlyings (spot(0), spot(1), ...): one
        /// spot, dividend yield and flat vol per asset, and their correlation
        /// matrix, row-major n x n. Used instead of spot/dividend/vol when
        /// `spots` has two or more entries.
        std::vector<double> spots;
        std::vector<double> dividends;
        std::vector<double> vols;
        std::vector<double> correlation;
        /// local_vol: the Dupire grid; slv: the leverage grid's axes.
        std::vector<double> K_grid;
        std::vector<double> T_grid;
        std::vector<double> sigma_loc_flat; ///< local_vol, K-major
        HestonParams heston;                ///< heston, slv
        std::vector<double> leverage_flat;  ///< slv, K-major on (K_grid, T_grid)
        /// local_vol on several underlyings: one Dupire grid per asset, in
        /// spots order (with spots, dividends and correlation above).
        std::vector<std::vector<double>> K_grids;
        std::vector<std::vector<double>> T_grids;
        std::vector<std::vector<double>> sigma_loc_flats;
        double max_dt = 1.0 / 52.0; ///< Euler step bound (every model but black_scholes)
        /// hull_white: the short rate (curve-fitted Hull-White) and its
        /// correlation with the equity; the discount curve as pillars
        /// (times, discount factors), or the flat `rate` when empty.
        double hw_mean_reversion = 0.03;
        double hw_sigma = 0.01;
        double hw_rho = 0.0;
        std::vector<double> curve_times;
        std::vector<double> curve_dfs;
    };

    /**
     * @brief The model a script is priced under. A script only describes a
     *        payoff; which dynamics its price depends on is read off it by
     *        script_analyzer.hpp, and model_advice.hpp's recommend() turns
     *        that into a choice (the caller may override it).
     *
     *  "black_scholes": one flat vol, simulated exactly; with `spots`,
     *                   several correlated underlyings, one flat vol each.
     *  "local_vol":     a Dupire surface in the shape calibrate_vol_surface
     *                   returns (K_grid, T_grid, sigma_loc_flat, K-major);
     *                   with `spots`, one surface per asset (K_grids, ...),
     *                   drivers correlated (MultiAssetLocalVolSimModel).
     *  "heston":        Heston stochastic volatility with calibrated
     *                   parameters (market/heston_calibration.hpp): the
     *                   Bates simulation model with no jumps, full-truncation
     *                   Euler.
     *  "hull_white":    the equity at a flat vol under stochastic Hull-White
     *                   rates fitted to a discount curve, correlated
     *                   (models/hybrid/hull_white_equity_sim_model.hpp);
     *                   df(T) reads the simulated curve.
     *  "slv":           stochastic-local volatility -- the calibrated Heston
     *                   dynamics times a leverage L(S, t) calibrated so that
     *                   the marginals are the Dupire surface's
     *                   (market/slv_calibration.hpp).
     */
    /// The spec's row-major correlation of n underlyings, checked: n x n,
    /// unit diagonal, entries in [-1, 1], symmetric.
    inline Eigen::MatrixXd correlation_matrix(const ScriptModelSpec &s, std::size_t n)
    {
        if (s.correlation.size() != n * n)
            throw InvalidInput("price_script: the correlation matrix must be n x n for n underlyings");
        Eigen::MatrixXd corr(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
            {
                const double c = s.correlation[i * n + j];
                if (i == j ? std::abs(c - 1.0) > 1e-12 : !(c >= -1.0 && c <= 1.0))
                    throw InvalidInput("price_script: a correlation matrix has a unit diagonal and entries in [-1, 1]");
                if (std::abs(c - s.correlation[j * n + i]) > 1e-12)
                    throw InvalidInput("price_script: the correlation matrix must be symmetric");
                corr(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = c;
            }
        return corr;
    }

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

        if (s.model == "black_scholes" && s.spots.size() > 1)
        {
            const std::size_t n = s.spots.size();
            if (s.dividends.size() != n || s.vols.size() != n)
                throw InvalidInput("price_script: spots, dividends and vols need one entry per underlying");
            for (double v : s.vols)
                if (!(v > 0.0))
                    throw InvalidInput("price_script: black_scholes needs every vol > 0");
            return std::make_unique<MultiAssetBSSimModel<T>>(
                to_T(s.spots), T(s.rate), to_T(s.dividends), to_T(s.vols), correlation_matrix(s, n));
        }
        if (s.model == "local_vol" && s.spots.size() > 1)
        {
            const std::size_t n = s.spots.size();
            if (s.dividends.size() != n || s.K_grids.size() != n || s.T_grids.size() != n ||
                s.sigma_loc_flats.size() != n)
                throw InvalidInput("price_script: local_vol on several underlyings needs one dividend and one "
                                   "grid (K_grids, T_grids, sigma_loc_flats) per underlying");
            std::vector<typename MultiAssetLocalVolSimModel<T>::Surface> surfaces;
            for (std::size_t i = 0; i < n; ++i)
                surfaces.push_back({s.K_grids[i], s.T_grids[i], to_T(s.sigma_loc_flats[i])});
            return std::make_unique<MultiAssetLocalVolSimModel<T>>(to_T(s.spots), T(s.rate), to_T(s.dividends),
                                                                   std::move(surfaces), correlation_matrix(s, n),
                                                                   s.max_dt);
        }
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
        if (s.model == "hull_white")
        {
            if (!(s.vol >= 0.0))
                throw InvalidInput("price_script: hull_white needs an equity vol >= 0");
            if (s.curve_times.size() != s.curve_dfs.size())
                throw InvalidInput("price_script: hull_white needs as many curve times as discount factors");
            DiscountCurve curve = s.curve_times.empty()
                                      ? DiscountCurve(s.rate)
                                      : DiscountCurve(s.curve_times, s.curve_dfs, CurveExtrapolation::FlatForward);
            return std::make_unique<HullWhiteEquitySimModel<T>>(
                T(s.spot), T(s.dividend), T(s.vol), HullWhiteCurveModel(s.hw_mean_reversion, s.hw_sigma, curve),
                s.hw_rho);
        }
        if (s.model == "slv")
            return std::make_unique<SLVSimModel<T>>(
                T(s.spot), T(s.rate), T(s.dividend), T(h.v0), T(h.kappa),
                T(h.theta), T(h.xi), T(h.rho), s.K_grid, s.T_grid,
                to_T(s.leverage_flat), s.max_dt);
        throw std::invalid_argument(
            "price_script: unknown model '" + s.model +
            "' (expected 'black_scholes', 'local_vol', 'heston', 'slv' or 'hull_white')");
    }

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_SCRIPT_MODEL_FACTORY_HPP
