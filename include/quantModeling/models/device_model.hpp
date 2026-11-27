#ifndef QM_MODELS_DEVICE_MODEL_HPP
#define QM_MODELS_DEVICE_MODEL_HPP

#include <optional>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/forward_curve.hpp"
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
        /// Black-Scholes, per asset: the parameters behind drift and
        /// vol_sqrt_dt, which a dual run differentiates (lot G3).
        std::vector<Real> divs, vols;
        mc::HestonParamsT<Real> heston{0.0, 0.0, 0.0, 0.0, 0.0};
        /// Bates's jump compensator k = E[Y - 1] (lambda = 0 on the device):
        /// dV/dlambda = -k (...) is not zero when k is, which the duals do not
        /// carry -- the risks then stay on the CPU.
        Real jump_k = 0.0;
        std::vector<Real> K, T_grid, grid; ///< local vol σ(K, T) or SLV leverage, K-major

        /// The simulation grid: one entry per step of the CPU model's
        /// sim_timeline(), in order.
        std::vector<Time> t;
        std::vector<int> draws; ///< 1 when the step draws (Black-Scholes skips t = 0)
        std::vector<int> event; ///< index of the product event written at this step, or -1
        /// Black-Scholes coefficients per step and asset (step-major), as the
        /// CPU model computed them.
        std::vector<Real> drift, vol_sqrt_dt;
        /// Multilevel's coarse path conditioned on the fine one (WP 26 §26.2):
        /// per step, the fine path's first sub-step's share of it. Empty:
        /// unconditional. Set by the engine on the coarse level's model; the
        /// kernel then draws two fine gaussians a step (scripting/ir_path.hpp,
        /// PathLayout::half).
        std::vector<Real> half_share;
        /// Local vol: Milstein on every step that reaches no product date,
        /// log-Euler on the others (#371, LocalVolSimModel::
        /// set_milstein_before_dates). Price kernels only: the risks stay on
        /// the CPU's tape until the IR differentiates the vol's slope.
        bool milstein_before_dates = false;

        /// Heston and SLV: how a price run steps the variance (#317). The
        /// risk kernels (duals, adjoints) always step it by Euler.
        mc::VarianceScheme variance_scheme = mc::VarianceScheme::Euler;

        /// WP 32 F4: the forward curve the CPU model steps on; none: a flat
        /// rate. `r` and `q` are then parallel shifts over it, and per step
        /// of `t` its carry rate c_k (the CPU model's CurveCarry table): the
        /// step's rate argument is c_k + r. The events' numeraires, discounts
        /// and forwards are the curve's, computed on the host with the CPU
        /// model's expressions. The risks per pillar: curve_pillars below.
        std::optional<ForwardCurve> curve;
        std::vector<Real> step_carry;
        /// The curve's pillar shifts a risk run differentiates (WP 32): set
        /// by the engine for the per-path adjoint, which then gives the
        /// risks per pillar as the CPU's tape does; 0, the rate and the
        /// dividend as parallel shifts only (the tangent, Vibrato).
        int curve_pillars = 0;

        int factors = 1; ///< Brownian draws per drawing step
        int stride = 1;  ///< draws per drawing step (>= factors)
    };

} // namespace quantModeling

#endif // QM_MODELS_DEVICE_MODEL_HPP
