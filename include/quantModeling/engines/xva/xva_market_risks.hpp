#ifndef QM_ENGINES_XVA_XVA_MARKET_RISKS_HPP
#define QM_ENGINES_XVA_XVA_MARKET_RISKS_HPP

#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"
#include "quantModeling/risk/regulatory/sa_cva.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @file xva_market_risks.hpp
 * @brief From the risks of the model's inputs to the risks of the market's
 *        quotes (blueprint/wp/23-xva.md §14.13, lot X8; WP 17 §11).
 *
 * xva_risks() differentiates to what the model is written in: zero rates at
 * the pillars, a and σ of Hull-White, hazard rates. A desk hedges with what
 * is quoted: par swap rates, swaption volatilities, credit spreads. The
 * quotes determine the inputs through three calibrations, each differentiated
 * at its solution rather than through its iterations:
 *
 *  - **the curve** is the bootstrap of the par swap rates: dz/dq is the
 *    derivative of the bootstrap itself;
 *  - **a and σ** minimise Σ w (model vol - quoted vol)²: by the implicit
 *    function theorem on the condition of the optimum, F(θ; z, m) =
 *    Jᵀ W (v - m) = 0 with v the model's vols and J = ∂v/∂θ:
 *    dθ/dm = (∂F/∂θ)⁻¹ JᵀW and dθ/dz = -(∂F/∂θ)⁻¹ ∂F/∂z. Unlike the
 *    Gauss-Newton Hessian of WP 17 (ADR-A9), ∂F/∂θ keeps the second
 *    derivatives of the vols: two parameters do not fit six quotes, and the
 *    residuals they leave are not small. A parameter at a bound of the
 *    calibration is frozen;
 *  - **the hazard rates** are the bootstrap of credit spreads: dh/ds.
 *
 * These Jacobians are those of small, smooth, deterministic functions with
 * closed forms; they are taken by central differences (error of order 1e-9),
 * where WP 17 records them on a tape. The expensive and noisy part, the
 * simulation, is the adjoint's. The map is linear: it is applied batch by
 * batch, so that every market risk keeps a Monte-Carlo error of its own.
 */

namespace quantModeling
{

    /// What the model's inputs were calibrated to.
    struct XvaMarketQuotes
    {
        /// Par OIS swap rates (tenor, rate): the curve is their bootstrap.
        std::vector<std::pair<Time, Real>> swap_rates;
        /// The swaption volatilities a and σ were fitted to.
        std::vector<SwaptionVolQuote> swaption_vols;
        int fixed_frequency = 1, float_frequency = 1;
        /// Set when the mean reversion was fixed and σ alone fitted.
        std::optional<Real> fixed_mean_reversion;
        /// The tenors at which each party's credit spread is quoted; its
        /// hazard curve must have one period per tenor. Empty: the hazard
        /// rates are reported as they are.
        std::vector<Time> counterparty_spread_tenors, own_spread_tenors;
        /// Recovery of the credit default swaps the spreads are those of.
        Real recovery = 0.4;
    };

    /// The sensitivity of every adjustment to one quote.
    struct XvaQuoteRisk
    {
        std::string label;
        /// Expiry of a swaption; 0 otherwise.
        Time expiry = 0.0;
        /// Tenor of the swap, of the swaption's swap, or of the spread.
        Time tenor = 0.0;
        /// The quote today.
        Real level = 0.0;
        /// d adjustment / d quote, per unit of the quote, indexed by XvaOutput.
        std::array<Estimate, kXvaOutputs> risk;
    };

    struct XvaMarketRisks
    {
        std::vector<XvaQuoteRisk> swap_rates;
        std::vector<XvaQuoteRisk> swaption_vols;
        std::vector<XvaQuoteRisk> counterparty_spreads;
        std::vector<XvaQuoteRisk> own_spreads;
        /// Losses given default and funding spreads: inputs and quotes at once.
        std::vector<XvaQuoteRisk> others;
        /// d a / d quote and d σ / d quote of each swaption volatility, and
        /// of each swap rate: how the calibration moves.
        std::vector<std::array<Real, 2>> calibration;
        std::vector<std::array<Real, 2>> calibration_to_swap_rates;
        /// True when the counterparty's risks are to credit spreads (its
        /// spread tenors were given), false when they are to hazard rates.
        bool counterparty_in_spreads = false;
    };

    /**
     * @param risks        from HullWhiteExposureEngine::xva_risks().
     * @param model        the model the engine ran under.
     * @param counterparty the credit curves the run was given.
     * @throws InvalidInput when the model's curve is not the bootstrap of the
     *         swap rates, when a hazard curve does not have one period per
     *         spread tenor, or without a swaption volatility.
     */
    XvaMarketRisks xva_market_risks(const XvaRisks &risks, const HullWhiteCurveModel &model,
                                    const CreditCurve &counterparty, const CreditCurve &own,
                                    const XvaMarketQuotes &quotes);

    /**
     * @brief The SA-CVA sensitivities of MAR50 from the market risks of the
     *        **unilateral** CVA: swap rates spread over the five prescribed
     *        tenors, all volatilities shifted together by a relative amount,
     *        counterparty spreads spread over their five tenors.
     */
    sa_cva::Sensitivities sa_cva_sensitivities(const XvaMarketRisks &risks, ba_cva::Sector sector,
                                               ba_cva::CreditQuality quality);

} // namespace quantModeling

#endif // QM_ENGINES_XVA_XVA_MARKET_RISKS_HPP
