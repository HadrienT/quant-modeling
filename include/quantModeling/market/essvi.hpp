#ifndef MARKET_ESSVI_HPP
#define MARKET_ESSVI_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi.hpp"
#include "quantModeling/market/svi_calibration.hpp"

#include <cstddef>
#include <limits>
#include <vector>

/**
 * @file essvi.hpp
 * @brief An implied-volatility surface free of static arbitrage by
 *        construction: extended SSVI, calibrated slice by slice
 *        (blueprint/wp/30-vol-surface.md, issue #381).
 *
 * One slice, with theta the at-the-money total variance, rho the slope's
 * correlation, psi = theta phi the at-the-money skew in total variance
 * (Gatheral & Jacquier 2014, eq. 4.1, with rho per maturity: Hendriks &
 * Martini's eSSVI):
 *
 *     w(k) = 1/2 (theta + rho psi k + sqrt(psi^2 k^2 + 2 rho psi theta k + theta^2)),
 *
 * k = ln(K / F_T). It is an SVI slice: b = psi / 2, rho, m = -rho theta / psi,
 * sigma = theta sqrt(1 - rho^2) / psi, a = theta (1 - rho^2) / 2 (essvi_to_svi).
 *
 * No butterfly arbitrage in a slice (Gatheral & Jacquier, Theorem 4.2, in psi):
 *
 *     psi (1 + |rho|) < 4,    psi^2 (1 + |rho|) <= 4 theta.
 *
 * No calendar arbitrage between slices i < j (Hendriks & Martini 2019, in
 * the corrected statement of Pasquazzi 2023, Proposition 4.14): with
 * theta_j > theta_i, the slices do not cross if and only if
 *
 *     |rho_j psi_j - rho_i psi_i| <= psi_j - psi_i                      (necessary)
 *
 * -- equivalently psi_j >= max(psi_i (1 - rho_i) / (1 - rho_j),
 * psi_i (1 + rho_i) / (1 + rho_j)) -- and moreover either
 *
 *     phi_j <= phi_i, phi = psi / theta,   or   (rho_j psi_j / psi_i - rho_i)^2
 *                                              <= (Theta - 1)(psi_j^2 / (psi_i^2 Theta) - 1),
 *
 * Theta = theta_j / theta_i. Corbetta, Cohort, Laachir & Martini (2019, §2.2)
 * state the necessary condition alone as sufficient; it is not (testESSVI
 * has two slices that meet it and cross by 93 % of theta at k = -0.58).
 * The calibration keeps the first branch, phi non-increasing: it is the
 * branch under which the interpolation below provably stays free of
 * calendar arbitrage (phi = psi / theta of two linear functions of T is
 * monotone between its ends), it never empties the feasible set (psi_j =
 * psi_i, rho_j = rho_i is always in it), and an equity skew's phi decays in
 * T like a power.
 *
 * The calibration (calibrate_essvi_slices) fits the slices in maturity
 * order, each under the butterfly bounds and the calendar bounds of the
 * slice before it -- Corbetta et al.'s "robust" search: rho on a grid then
 * refined, and for each rho the other parameters by bounded one-dimensional
 * searches, no starting point to guess. Unlike their first-order anchor
 * (theta from one quote, §2.1), theta is a free parameter here, bounded
 * below by the slice before: a single noisy at-the-money quote does not fix
 * it. A slice whose feasible set is empty given the slices before is left
 * out and reported.
 *
 * The surface between slices (ESSVISurface) interpolates theta, psi and
 * rho psi linearly in T, which keeps both conditions (Corbetta et al., §5.1,
 * and phi monotone as above);
 * before the first slice theta and psi scale with T / T_1 at rho_1 (§5.2);
 * past the last, theta grows on at the last segment's slope, psi and rho
 * held (§5.3). Its local variance is Dupire's on the analytic derivatives,
 * positive wherever the conditions hold.
 */

namespace quantModeling
{

    struct ESSVIParams
    {
        Real theta = 0.04; ///< at-the-money total variance
        Real rho = 0.0;
        Real psi = 0.1; ///< theta phi(theta)
    };

    Real essvi_total_variance(Real k, const ESSVIParams &p) noexcept;
    Real essvi_total_variance_dk(Real k, const ESSVIParams &p) noexcept;
    Real essvi_total_variance_dk2(Real k, const ESSVIParams &p) noexcept;

    /// The same slice as raw SVI (a, b, rho, m, sigma): what the slice
    /// reports and the front end draw. psi > 0, |rho| < 1.
    SVIParams essvi_to_svi(const ESSVIParams &p) noexcept;

    /// Gatheral & Jacquier's butterfly bounds hold.
    bool essvi_butterfly_free(const ESSVIParams &p) noexcept;
    /// The calendar conditions above hold from `before` to `after`, on the
    /// phi-non-increasing branch the calibration keeps.
    bool essvi_calendar_free(const ESSVIParams &before, const ESSVIParams &after) noexcept;

    struct ESSVISliceFit
    {
        Real ttm = 0.0;
        ESSVIParams params;
        Real rmse = std::numeric_limits<Real>::quiet_NaN();           ///< weighted, in implied vol
        Real worst_residual = std::numeric_limits<Real>::quiet_NaN(); ///< in implied vol
        std::size_t n_quotes = 0;
        /// Fitted under the bounds of the slices before it. False: its
        /// feasible set was empty, or it had too few quotes; it is not in
        /// the surface.
        bool feasible = false;
    };

    struct ESSVICalibrationSettings
    {
        int rho_grid = 41;          ///< rho sampled on (-rho_max, rho_max)
        int refine_iterations = 40; ///< golden-section steps around the best rho
        Real rho_max = 0.995;
        /// theta searched in [theta_lo, theta_atm (1 + theta_span)], theta_lo
        /// the slice before's (or theta_atm (1 - theta_span) for the first),
        /// theta_atm the quotes' at-the-money total variance.
        Real theta_span = 0.6;
        int inner_iterations = 60; ///< golden-section steps for theta and Brent steps for psi
        std::size_t min_quotes = 6;
        /// The at-the-money total variance's term structure first (#381):
        /// each maturity's theta estimated near the money with its standard
        /// error, then a smooth forward variance (monotone_theta) whose
        /// integral is closest to them, and the slices fitted with theta on
        /// that curve. Two expiries a fortnight
        /// apart whose quotes contradict each other then no longer force a
        /// forward variance near zero between them -- arbitrage-free, and a
        /// local vol of 1 %. False: theta free in each slice (calendar bound
        /// from the slice before only).
        bool smooth_theta = true;
        Real min_forward_vol = 0.05;
        /// monotone_theta's roughness weight, relative to the data's (see there).
        Real forward_smoothing = 0.1;
        /// The surface's pillars (essvi_surface_pillars): consecutive ones at
        /// least this far apart in relative maturity, as SVI's slice selection
        /// (SurfaceSliceSelection::min_relative_gap). Every feasible slice is
        /// fitted; only the pillars build the surface and its local vol.
        Real surface_min_relative_gap = 0.25;
    };

    /**
     * Which feasible fits are the surface's pillars (WP 30 E4): the shortest,
     * then each one at least (1 + min_relative_gap) times the last kept in
     * maturity, and the longest always (it replaces the last kept, unless that
     * is the shortest). The others stay calibrated -- each fit is conditioned
     * on the feasible one before it -- but build neither the surface nor the
     * local vol.
     *
     * Why: Dupire reads dw/dT, and two pillars a day apart (SPX has an expiry
     * every few days) make it the difference of two fits' noise over a day:
     * on SPY 2026-11-26 the one-year local vol jumped from 0.31 to 0.18
     * between neighbouring columns. And forty pillars leave too few grid
     * columns to give each segment its two edges (local_vol_maturities), so
     * the grid fell back to equal steps starting at 0.08 years, past the
     * first month's steep put wing: a three-month put 20 % out of the money
     * priced 41 % under its own surface.
     */
    std::vector<bool> essvi_surface_pillars(const std::vector<ESSVISliceFit> &fits, Real min_relative_gap);

    /**
     * The slices, in the order given (`ttms` increasing), each fitted under
     * the conditions above to its quotes by weighted least squares in
     * implied vol, sum w_i (iv(k_i) - market_iv_i)^2.
     */
    std::vector<ESSVISliceFit> calibrate_essvi_slices(const std::vector<std::vector<SVISliceQuote>> &quotes,
                                                      const std::vector<Real> &ttms,
                                                      const ESSVICalibrationSettings &settings = {});

    /// The at-the-money total variances' term structure (see
    /// ESSVICalibrationSettings::smooth_theta). Piecewise-constant forward
    /// variances f_j >= v_min on [T_{j-1}, T_j] (T_{-1} = 0), theta_i their
    /// integral, minimising
    ///
    ///   sum_i weight_i (theta_i - theta_hat_i)^2
    ///     + lambda sum_j (f_j - f_{j-1})^2 / ((d_j + d_{j-1}) / 2),
    ///
    /// d_j = T_j - T_{j-1}, lambda = smoothing x mean(weight) x mean(d)^3 (the
    /// two terms' units match): a convex quadratic under lower bounds, solved
    /// exactly by coordinate descent. A maturity that contradicts its
    /// neighbours is outvoted by them and the curve stays smooth, where an
    /// isotonic regression would pool them on a flat stretch of minimum
    /// forward variance. Strictly increasing.
    std::vector<Real> monotone_theta(const std::vector<Real> &T, const std::vector<Real> &theta_hat,
                                     const std::vector<Real> &weight, Real v_min, Real smoothing = 0.1);

    /**
     * The surface through the feasible slices (see the file comment). Same
     * interface as SVISurface where the local-vol grid reads it.
     */
    class ESSVISurface
    {
      public:
        /// The feasible fits of calibrate_essvi_slices, at least one.
        explicit ESSVISurface(std::vector<ESSVISliceFit> fits);

        Real ttm_min() const noexcept { return slices_.front().ttm; }
        Real ttm_max() const noexcept { return slices_.back().ttm; }
        const std::vector<ESSVISliceFit> &slices() const noexcept { return slices_; }

        /// The slice's parameters at any T > 0, and their slopes in T.
        ESSVIParams params(Real T) const noexcept;
        void params_dT(Real T, Real &theta_T, Real &psi_T, Real &rho_psi_T) const noexcept;

        Real total_variance(Real k, Real T) const noexcept;
        Real total_variance_dk(Real k, Real T) const noexcept;
        Real total_variance_dk2(Real k, Real T) const noexcept;
        /// Piecewise-constant slopes in T: on a slice, the segment after it.
        Real total_variance_dT(Real k, Real T) const noexcept;
        Real implied_vol(Real k, Real T) const noexcept;

        /// SVISurface's bracket, for the grid's column bands: the slices T
        /// falls between (lower kZeroSlice before the first).
        static constexpr std::size_t kZeroSlice = static_cast<std::size_t>(-1);
        struct Bracket
        {
            std::size_t lower;
            std::size_t upper;
            Real lambda;
            Real width;
        };
        Bracket bracket(Real T) const noexcept;

      private:
        std::vector<ESSVISliceFit> slices_;
    };

} // namespace quantModeling

#endif
