#ifndef QM_MODELS_DEVICE_MODEL_HPP
#define QM_MODELS_DEVICE_MODEL_HPP

#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/path_steps.hpp"

/**
 * @file device_model.hpp
 * @brief A simulation model as plain data, for the GPU script engine
 *        (blueprint/wp/19-gpu.md §4, lot G2).
 *
 * The closed set of models the device simulates. A CPU model fills one of
 * these after init() (ISimulationModel::describe_device); the kernel steps it
 * with the functors of path_steps.hpp, or with the per-step coefficients the
 * CPU model itself precomputed (Black-Scholes), so that a path on the GPU is
 * the CPU's path up to the last ulps of exp and sqrt.
 *
 * Draws: drawing step d reads gaussians d * stride + f, f < factors -- the
 * CPU model's own layout (BrownianLayout), so the same Philox stream feeds
 * both.
 */

namespace quantModeling
{

    struct DeviceModel
    {
        enum class Kind : int
        {
            BlackScholes, ///< flat rate; one or several correlated assets
            LocalVol,
            Heston, ///< Bates without jumps: the scripts' Heston
            SLV
        };

        Kind kind = Kind::BlackScholes;
        int n_assets = 1;
        Real r = 0.0;
        Real q = 0.0;           ///< single-asset diffusions
        std::vector<Real> s0;   ///< n_assets
        std::vector<Real> chol; ///< n_assets² row-major, Black-Scholes with n_assets > 1
        mc::HestonParamsT<Real> heston{0.0, 0.0, 0.0, 0.0, 0.0};
        std::vector<Real> K, T_grid, grid; ///< local vol σ(K, T) or SLV leverage, K-major

        /// The simulation grid: one entry per step of the CPU model's
        /// sim_timeline(), in order.
        std::vector<Time> t;
        std::vector<int> draws; ///< 1 when the step draws (Black-Scholes skips t = 0)
        std::vector<int> event; ///< index of the product event written at this step, or -1
        /// Black-Scholes coefficients per step and asset (step-major), as the
        /// CPU model computed them.
        std::vector<Real> drift, vol_sqrt_dt;

        int factors = 1; ///< Brownian draws per drawing step
        int stride = 1;  ///< draws per drawing step (>= factors)
    };

} // namespace quantModeling

#endif // QM_MODELS_DEVICE_MODEL_HPP
