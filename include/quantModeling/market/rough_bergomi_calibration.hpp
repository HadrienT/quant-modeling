#ifndef QM_MARKET_ROUGH_BERGOMI_CALIBRATION_HPP
#define QM_MARKET_ROUGH_BERGOMI_CALIBRATION_HPP

#include "quantModeling/engines/mc/rough_bergomi_surface.hpp"
#include "quantModeling/market/calibration/levenberg_marquardt.hpp"

#include <vector>

namespace quantModeling
{

    /// A market implied vol to fit: maturity, log-moneyness ln(K / F), vol.
    struct RoughBergomiTarget
    {
        Time ttm;
        Real k;
        Real implied_vol;
    };

    struct RoughBergomiCalibration
    {
        RoughBergomiParams params;
        ForwardVarianceCurve xi;
        calibration::CalibrationReport report; ///< residuals in vol points (decimal)
        Real iv_rmse = 0.0;                    ///< over the priced quotes, vol points
        Real iv_worst = 0.0;
        std::size_t n_quotes = 0;
        std::size_t n_unpriced = 0;
        int xi_floored = 0;           ///< forward variances floored (calendar arbitrage in the ATM vols)
        std::vector<Real> model_vols; ///< per target (NaN where unpriced)
        /// Monte-Carlo error of each model vol, from the price's standard
        /// error through the Black vega: what the fit cannot resolve.
        std::vector<Real> model_vol_errors;
    };

    /**
     * @brief Calibrate rough Bergomi (H, η, ρ) to a surface of implied vols
     *        (blueprint: etc/roadmap.md §1c).
     *
     * ξ0(t) = E[V_t] is not a free parameter: it is the market's forward
     * variance, from the **variance-swap** total variance w_VS(T) =
     * −2 E[ln S_T/F] at a few maturities (`ttm`, `total_variance`),
     * replicated from the smile (Carr-Madan). Not the at-the-money variance:
     * under a negative skew the variance swap sits well above it, and a
     * model on the ATM curve is biased low at every strike. (H, η, ρ) are fitted by
     * Levenberg-Marquardt within H ∈ [0.02, 0.45], η ∈ [0.2, 5],
     * ρ ∈ [−0.99, 0.2], on residuals model − market implied vol.
     *
     * Every objective call prices the whole surface on the same Monte-Carlo
     * paths (common random numbers, rough_bergomi_surface): the fit is a
     * smooth deterministic function of the parameters, and its Monte-Carlo
     * error is reported per quote rather than hidden in the residuals.
     */
    RoughBergomiCalibration calibrate_rough_bergomi(const std::vector<RoughBergomiTarget> &targets,
                                                    const std::vector<Time> &ttm,
                                                    const std::vector<Real> &total_variance,
                                                    const RoughBergomiSurfaceSettings &mc = {},
                                                    RoughBergomiParams initial = {0.1, 1.5, -0.7});

} // namespace quantModeling

#endif
