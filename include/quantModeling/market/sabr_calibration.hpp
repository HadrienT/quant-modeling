#ifndef MARKET_SABR_CALIBRATION_HPP
#define MARKET_SABR_CALIBRATION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/market/calibration/objective_function.hpp"
#include "quantModeling/models/equity/sabr.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /// One market observation to fit: a strike and its quoted Black implied
    /// vol, for a single maturity slice at a known forward.
    struct SABRSliceQuote
    {
        Real strike = 0.0;
        Real market_iv = 0.0;
        Real weight = 1.0; ///< shapes the fit only -- see calibration::ObjectiveFunction's convention
    };

    /**
     * Weighted least-squares objective for one SABR slice: residual_i =
     * sabr_implied_vol(K_i) - market_iv_i, in implied-vol points, matching
     * the project's calibration-report convention (see
     * market/svi_calibration.hpp's SVISliceObjective, the same pattern).
     *
     * beta is fixed, not calibrated -- see SABRParams' doc comment: a single
     * smile snapshot cannot jointly identify alpha and beta, so only
     * (alpha, rho, nu) are free parameters here, packed as [alpha, rho, nu].
     *
     * Fits the closed-form sabr_implied_vol, not the arbitrage-free PDE:
     * the PDE is the tool for pricing once a smile is trusted, not for
     * calibrating one -- one PDE solve per residual evaluation would make
     * every Levenberg-Marquardt iteration cost as much as an entire slice
     * calibration does today with the closed form. This means a calibrated
     * fit is not itself guaranteed arbitrage-free; feed its parameters to
     * market/sabr_pde.hpp when that matters.
     */
    class SABRSliceObjective final : public calibration::ObjectiveFunction
    {
      public:
        SABRSliceObjective(std::vector<SABRSliceQuote> quotes, Real forward, Real ttm, Real beta);

        std::size_t num_params() const override { return 3; }
        std::size_t num_residuals() const override { return quotes_.size(); }

        std::vector<Real> residuals(const std::vector<Real> &params) const override;
        std::vector<Real> weights() const override;
        std::vector<Real> lower_bounds() const override { return lower_; }
        std::vector<Real> upper_bounds() const override { return upper_; }

        static SABRParams unpack(const std::vector<Real> &params, Real beta) noexcept;
        static std::vector<Real> pack(const SABRParams &p);

        /// alpha0 backed out from the quote closest to the forward (ATM
        /// implied vol ~ alpha / F^(1-beta) to leading order), rho0 = 0,
        /// nu0 a generic starting scale.
        std::vector<Real> initial_guess() const;

        /// Perturbations of initial_guess() over rho0 and the nu0 scale --
        /// same multi-start rationale as SVISliceObjective's (see its doc
        /// comment): a single start is not reliable enough on a real chain.
        std::vector<std::vector<Real>> initial_guess_candidates() const;

      private:
        std::vector<SABRSliceQuote> quotes_;
        Real forward_;
        Real ttm_;
        Real beta_;
        std::vector<Real> lower_;
        std::vector<Real> upper_;
    };

    struct SABRSliceCalibration
    {
        Real ttm = 0.0;
        SABRParams params;
        calibration::CalibrationReport report;
    };

    /**
     * Fit SABR (alpha, rho, nu; beta fixed) to one maturity slice's quotes.
     * Runs Levenberg-Marquardt from every candidate in
     * SABRSliceObjective::initial_guess_candidates() and keeps the one with
     * the lowest RMSE.
     */
    SABRSliceCalibration calibrate_sabr_slice(
        std::vector<SABRSliceQuote> quotes, Real forward, Real ttm, Real beta,
        const calibration::LevenbergMarquardtSettings &settings = {});

} // namespace quantModeling

#endif
