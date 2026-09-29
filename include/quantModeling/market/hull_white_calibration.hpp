#ifndef QM_MARKET_HULL_WHITE_CALIBRATION_HPP
#define QM_MARKET_HULL_WHITE_CALIBRATION_HPP

#include "quantModeling/market/calibration/levenberg_marquardt.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <optional>
#include <vector>

namespace quantModeling
{

    /// An at-the-money swaption quoted in normal (Bachelier) vol: expiry and
    /// tenor in years, the vol in rate units (0.01 = 100bp).
    struct SwaptionVolQuote
    {
        Time expiry;
        Time tenor;
        Real normal_vol;
        Real weight = 1.0; ///< shapes the fit only
    };

    struct HullWhiteCalibration
    {
        Real mean_reversion = 0.0;
        Real sigma = 0.0;
        /// rmse and worst_residual in basis points of normal vol.
        calibration::CalibrationReport report;
        std::vector<Real> strikes;     ///< the ATM forward swap rate of each quote
        std::vector<Real> market_vols; ///< normal vol, rate units
        std::vector<Real> model_vols;  ///< the calibrated model's, same unit
    };

    /// The model's ATM normal vol for one quote: Hull-White price inverted
    /// through Bachelier on the quote's own forward and annuity.
    Real hull_white_atm_normal_vol(const HullWhiteCurveModel &model, Time expiry, Time tenor,
                                   int fixed_frequency = 1, int float_frequency = 4);

    /**
     * @brief Calibrate Hull-White (a, σ) to a grid of ATM swaption normal
     *        vols (blueprint/wp/21-rates.md §3), on a fixed discount and
     *        projection curve.
     *
     * Residual = model normal vol − market normal vol, in basis points, so
     * the report reads in bp of vol. Levenberg-Marquardt within
     * a ∈ [1e-4, 1], σ ∈ [1e-5, 0.2]. With `fixed_mean_reversion`, only σ is
     * fitted: a constant-parameter Hull-White cannot match a whole cube, and
     * desks commonly fix a (from a historical or Bermudan-vs-European
     * argument) and fit σ to the co-terminal swaptions the trade depends on.
     */
    HullWhiteCalibration calibrate_hull_white(const DiscountCurve &discount, const DiscountCurve &projection,
                                              const std::vector<SwaptionVolQuote> &quotes,
                                              int fixed_frequency = 1, int float_frequency = 4,
                                              std::optional<Real> fixed_mean_reversion = std::nullopt);

} // namespace quantModeling

#endif // QM_MARKET_HULL_WHITE_CALIBRATION_HPP
