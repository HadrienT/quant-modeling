#ifndef MARKET_SVI_CALIBRATION_HPP
#define MARKET_SVI_CALIBRATION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/market/calibration/objective_function.hpp"
#include "quantModeling/market/raw_vol_surface.hpp"
#include "quantModeling/market/svi.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /// One market observation to fit, already in the (log-moneyness, vol)
    /// coordinates a single SVI slice is calibrated in.
    struct SVISliceQuote
    {
        Real log_moneyness = 0.0; ///< k = ln(K / F_T)
        Real market_iv = 0.0;
        Real weight = 1.0; ///< shapes the fit only -- see ObjectiveFunction's convention
    };

    /**
     * Weighted least-squares objective for one SVI slice: residual_i =
     * svi_implied_vol(k_i) - market_iv_i, i.e. residuals in implied-vol
     * points, per the project's calibration-report convention. Parameters are
     * packed as [a, b, rho, m, sigma].
     */
    class SVISliceObjective final : public calibration::ObjectiveFunction
    {
      public:
        SVISliceObjective(std::vector<SVISliceQuote> quotes, Real ttm);

        std::size_t num_params() const override { return 5; }
        std::size_t num_residuals() const override { return quotes_.size(); }

        std::vector<Real> residuals(const std::vector<Real> &params) const override;
        std::vector<Real> weights() const override;
        std::vector<Real> lower_bounds() const override { return lower_; }
        std::vector<Real> upper_bounds() const override { return upper_; }

        static SVIParams unpack(const std::vector<Real> &params) noexcept;
        static std::vector<Real> pack(const SVIParams &p);

        /// A data-driven starting point: m0 = mean(k), sigma0 from the spread
        /// of k, b0 small and positive, rho0 = 0 (no skew assumed a priori),
        /// a0 set so the slice minimum roughly matches the smallest observed
        /// total variance.
        std::vector<Real> initial_guess() const;

        /// Several perturbations of initial_guess() (varying rho0 and the
        /// sigma0 scale), for calibrate_svi_slice's multi-start search. SVI
        /// is not convex: a single start can converge to a poor local
        /// optimum (or stall at a bound) on a real, noisy chain, even when
        /// it fits a clean synthetic one perfectly -- found by testing
        /// against a real GOOGL chain, where most slices failed to
        /// converge from the single data-driven start and several ended up
        /// outside the butterfly-arbitrage-free region entirely.
        std::vector<std::vector<Real>> initial_guess_candidates() const;

      private:
        std::vector<SVISliceQuote> quotes_;
        Real ttm_;
        std::vector<Real> lower_;
        std::vector<Real> upper_;
    };

    struct SVISliceCalibration
    {
        Real ttm = 0.0;
        SVIParams params;
        calibration::CalibrationReport report;
        bool butterfly_arbitrage_free = false;
    };

    /**
     * Fit SVI to one maturity slice's cleaned quotes.
     *
     * Runs Levenberg-Marquardt from every candidate in
     * SVISliceObjective::initial_guess_candidates() and keeps the best --
     * scored by RMSE, with a penalty against any candidate that lands
     * outside the butterfly-arbitrage-free region, so a slightly worse but
     * arbitrage-free fit is preferred over a marginally tighter one that
     * isn't. A single start is not enough on a real chain (see
     * initial_guess_candidates' doc comment).
     */
    SVISliceCalibration calibrate_svi_slice(
        std::vector<SVISliceQuote> quotes, Real ttm,
        const calibration::LevenbergMarquardtSettings &settings = {});

    /**
     * Build SVISliceQuote observations directly from a RawVolSurface's
     * cleaned quotes at one maturity (matched to `ttm` within
     * `ttm_tolerance`, since a real chain's quotes for one expiry share one
     * TTM value by construction), weighted by Black-Scholes vega evaluated at
     * each quote's own market IV -- so the ATM region, which carries the most
     * information about the level of the smile, is not swamped by noisy wing
     * quotes with large absolute price but tiny vega.
     */
    std::vector<SVISliceQuote> svi_quotes_from_raw_surface(
        const RawVolSurface &surface, Real ttm, Real ttm_tolerance = 1e-9);

    /**
     * Calendar-arbitrage check between two already-calibrated adjacent
     * slices (shorter.ttm < longer.ttm): total variance must be
     * non-decreasing in maturity at every log-moneyness, i.e.
     * w_shorter(k) <= w_longer(k) for every k sampled on [k_min, k_max].
     */
    bool svi_slices_are_calendar_arbitrage_free(
        const SVISliceCalibration &shorter, const SVISliceCalibration &longer,
        Real k_min, Real k_max, std::size_t n_grid = 200);

} // namespace quantModeling

#endif
