#ifndef MARKET_HESTON_CALIBRATION_HPP
#define MARKET_HESTON_CALIBRATION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/market/calibration/objective_function.hpp"
#include "quantModeling/models/equity/heston.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /// One point of the implied-vol surface to fit: a strike, its maturity
    /// and its Black implied vol.
    struct HestonCalibrationQuote
    {
        Real strike = 0.0;
        Real ttm = 0.0;
        Real implied_vol = 0.0;
        Real weight = 1.0; ///< shapes the fit only -- see calibration::ObjectiveFunction's convention
    };

    /**
     * Weighted least-squares objective for Heston on a whole surface (every
     * maturity at once: Heston's five parameters are shared by all of them).
     *
     * residual_i = (heston_price_i - market_price_i) / market_vega_i, where
     * the market price is Black-76 at the quoted vol and each quote is priced
     * as its out-of-the-money option (put below the forward, call above).
     * To first order this is the model's implied vol minus the market's, so
     * the fit reads in vol points like the SVI and SABR calibrations, while
     * each residual costs a COS price instead of an implied-vol inversion
     * (roadmap chantier 0: calibrate in price, weight by vega). The exact
     * implied-vol error is measured once at the end (HestonCalibration).
     *
     * Parameters are packed [v0, kappa, theta, xi, rho]. Quotes sharing a
     * maturity are priced by one heston_cos_prices call.
     */
    class HestonSurfaceObjective final : public calibration::ObjectiveFunction
    {
      public:
        HestonSurfaceObjective(std::vector<HestonCalibrationQuote> quotes, Real spot,
                               Real rate, Real dividend,
                               const HestonCOSSettings &cos = {});

        std::size_t num_params() const override { return 5; }
        std::size_t num_residuals() const override { return n_quotes_; }
        std::size_t num_maturities() const { return slices_.size(); }

        std::vector<Real> residuals(const std::vector<Real> &params) const override;
        std::vector<Real> weights() const override;
        std::vector<Real> lower_bounds() const override;
        std::vector<Real> upper_bounds() const override;

        static HestonParams unpack(const std::vector<Real> &params) noexcept;
        static std::vector<Real> pack(const HestonParams &p);

        /// v0 = theta = the variance of the quote nearest the money at the
        /// shortest maturity, crossed with a small grid of mean-reversion
        /// speeds, vols of vol and correlations: a single start is not
        /// reliable (the kappa/xi valley of the Heston fit is flat).
        std::vector<std::vector<Real>> initial_guess_candidates() const;

        /// Model implied vol minus market implied vol, per quote, in input
        /// order (NaN where the model price has no Black-76 implied vol).
        std::vector<Real> implied_vol_errors(const HestonParams &p) const;

      private:
        struct Slice
        {
            Real ttm;
            Real forward;
            Real discount;
            std::vector<std::size_t> index; ///< positions in the input order
            std::vector<Real> strikes;
        };

        std::vector<Real> model_prices(const HestonParams &p, bool calls_only) const;

        std::vector<HestonCalibrationQuote> quotes_;
        std::vector<Slice> slices_;
        std::vector<Real> market_price_;
        std::vector<Real> market_vega_;
        std::vector<bool> is_call_;
        std::size_t n_quotes_;
        HestonCOSSettings cos_;
    };

    struct HestonCalibration
    {
        HestonParams params;
        calibration::CalibrationReport report; ///< the winning start's LM report (vega-scaled residuals)
        Real iv_rmse = 0.0;                    ///< exact implied-vol RMSE, vol points
        Real iv_worst = 0.0;                   ///< exact worst |implied-vol error|, vol points
        std::size_t n_quotes = 0;
        std::size_t n_unpriced = 0; ///< quotes whose model price has no Black-76 implied vol (left out of the RMSE)
        std::size_t n_maturities = 0;
        std::size_t n_starts = 0;
        bool feller = false; ///< 2 kappa theta > xi^2 (informational, see heston.hpp)
    };

    /**
     * Fit Heston (v0, kappa, theta, xi, rho) to a surface of implied vols by
     * Levenberg-Marquardt, from every candidate start, keeping the lowest
     * cost. `spot`, `rate` and `dividend` set each maturity's forward and
     * discount factor (flat rate and yield, the convention of the vol-surface
     * pipeline the quotes usually come from).
     *
     * Throws InvalidInput on an empty surface or a non-positive input.
     */
    HestonCalibration calibrate_heston(
        std::vector<HestonCalibrationQuote> quotes, Real spot, Real rate, Real dividend,
        const calibration::LevenbergMarquardtSettings &settings = {},
        const HestonCOSSettings &cos = {});

} // namespace quantModeling

#endif
