#ifndef MARKET_SLV_CALIBRATION_HPP
#define MARKET_SLV_CALIBRATION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/models/equity/heston.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quantModeling
{

    /**
     * @brief A calibrated leverage function on the strikes of the Dupire
     *        local-vol surface it was calibrated against and on a finer time
     *        grid: every calibration substep (SLVCalibrationSettings::max_dt),
     *        the surface's maturities among them (issue #227).
     *
     * K-major layout, identical to models/volatility.hpp's GridLocalVol and
     * models/equity/local_vol_sim_model.hpp's sigma_loc: leverage[i*n_T + j]
     * corresponds to (K_grid[i], T_grid[j]) -- this struct's T_grid, which a
     * caller passes to SLVSimModel in place of the surface's.
     */
    struct SLVLeverageGrid
    {
        std::vector<Real> K_grid;
        std::vector<Real> T_grid;
        std::vector<Real> leverage;
        /// Share of the particles, over every column, that sat in a bucket
        /// whose leverage was held at the floor or cap: how much of the
        /// distribution the surface is not matched on. The share of grid
        /// points says little, since most clamped points are in the tails no
        /// particle reaches (issue #113).
        Real clamped_mass = 0.0;
    };

    struct SLVCalibrationSettings
    {
        std::size_t n_particles = 50000;
        std::uint64_t seed = 1;
        /// Euler substep inside one surface interval, and the spacing of the
        /// returned leverage's columns.
        Time max_dt = 1.0 / 50.0;
        Real variance_floor = 1e-8;
        Real leverage_floor = 1e-3; ///< guards a near-empty bucket's estimate
        Real leverage_cap = 5.0;    ///< guards a near-empty bucket's estimate
        /// E[v_t | S_t = K] is a kernel regression on the particles (quartic
        /// kernel), with Guyon & Henry-Labordere's bandwidth
        /// h = kernel_scale * sigma_ATM(t) * s0 * sqrt(max(t, kernel_min_time)) * N^(-1/5).
        /// A nearest-strike average is noisy from one strike to the next (a
        /// few hundred particles each), and the pathwise greeks of SLV read
        /// the leverage's slope in the spot: that noise made them
        /// meaningless. 0 restores the nearest-strike average.
        Real kernel_scale = 1.5;
        Time kernel_min_time = 0.25;
    };

    /**
     * @brief Calibrates a leverage function L(S,t) so that Heston dynamics
     *        multiplied by L reproduce the *given* Dupire local-vol surface
     *        exactly (in the Gyongy-mimicking sense) -- issue #37's SLV item:
     *        "combine l'exactitude de calibration de Dupire ... avec la
     *        dynamique de Heston ... ce que font beaucoup de desks".
     *
     *   dS/S = (r - q) dt + L(S,t) sqrt(v) dW1
     *   dv   = kappa(theta - v) dt + xi sqrt(v) dW2,   dW1 dW2 = rho dt
     *
     * By Gyongy's mimicking theorem, the SDE above reproduces the
     * marginal law of a pure local-vol process (and therefore its implied
     * vol surface) at every S and t if and only if
     *
     *   L(S,t)^2 = sigma_loc(S,t)^2 / E[v_t | S_t = S]
     *
     * E[v_t | S_t = S] has no closed form, so this is calibrated by the
     * particle method (Guyon & Henry-Labordere 2012's "smile calibration
     * problem solved", with their kernel regression of E[v|S] and their
     * bandwidth, SLVCalibrationSettings::kernel_scale): simulate n_particles
     * forward through the T_grid one column at a time, using the leverage
     * already calibrated at the *previous* column held constant over the
     * step (the standard resolution of the method's chicken-and-egg
     * problem -- L at column j needs the particle distribution at column
     * j, which needs L over the step *into* column j; using column j-1's
     * already-known L is exact in the limit T_grid -> 0 and is what makes
     * the forward sweep well-defined at all), then estimate E[v|S] at every
     * strike of K_grid by a quartic-kernel average of the particles' v
     * (strikes no particle's kernel reaches take the nearest estimate).
     *
     * `sigma_loc` must be on the *same* (K_grid, T_grid) as the leverage
     * grid this returns -- typically market::build_local_vol_grid's own
     * output, passed straight through (same K-major layout).
     *
     * The returned grid's *last* T column is computed but structurally
     * unused by SLVSimModel's lookup, mc::leverage_staircase() (see its doc
     * comment in models/equity/path_steps.hpp):
     * it is only ever read for t > T_grid.back(), which never happens once
     * a lookup time is clamped to the grid. Pricing is unaffected -- the
     * final interval is fully covered by the second-to-last column -- but
     * do not expect a nonzero AAD risk on the last column's grid points.
     */
    SLVLeverageGrid calibrate_slv_leverage(
        Real s0, Real r, Real q, const HestonParams &heston,
        const std::vector<Real> &K_grid, const std::vector<Real> &T_grid,
        const std::vector<Real> &sigma_loc,
        const SLVCalibrationSettings &settings = {},
        /// The particles' carry over each substep, ln F(t1)/F(t0) (WP 32
        /// F5), as SLVSimModel steps on it; null: the flat r − q.
        const ForwardCurve *forwards = nullptr);

} // namespace quantModeling

#endif
