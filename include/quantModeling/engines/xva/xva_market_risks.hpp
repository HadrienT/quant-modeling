#ifndef QM_ENGINES_XVA_XVA_MARKET_RISKS_HPP
#define QM_ENGINES_XVA_XVA_MARKET_RISKS_HPP

#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/models/rates/cross_currency_hull_white.hpp"
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
     * @brief The risks of a two-factor run (CrossCurrencyExposureEngine::
     *        xva_risks) at a fixed **market** volatility of the exchange rate
     *        or of the asset, in place (§14.17).
     *
     * The run differentiates to σ_S, the volatility of the spot in the
     * model. A desk does not quote σ_S: it was set so that an option of
     * `expiry` has its market Black volatility (calibrated_fx_volatility),
     * and so it moves with everything that volatility's formula holds — a
     * and σ of both rates, the three correlations. With the market
     * volatility held fixed,
     *
     *   d/dθ = ∂/∂θ + ∂/∂σ_S × ∂σ_S/∂θ,   d/dvol = ∂/∂σ_S × ∂σ_S/∂vol.
     *
     * σ_S is the root of a quadratic: its derivatives are taken by central
     * differences of that closed form. Applied batch by batch, so every risk
     * keeps a Monte-Carlo error of its own. After the call the factor
     * SpotVolatility is the market volatility (label and level changed).
     *
     * @throws InvalidInput when the risks are not those of a two-factor run,
     *         or when the model's σ_S is not the calibration to this quote.
     */
    void fold_spot_volatility_calibration(XvaRisks &risks, const CrossCurrencyHullWhiteModel &model,
                                          Time expiry, Real market_volatility);

    /// One factor of a run as a quote risk: what is both an input of the
    /// model and something read on a market (a spot, a dividend yield), or
    /// an estimate reported as it is (a correlation).
    XvaQuoteRisk factor_risk(const XvaRisks &risks, std::size_t factor);

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
