#ifndef QM_RISK_INITIAL_MARGIN_HPP
#define QM_RISK_INITIAL_MARGIN_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/bucketed_regression.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace quantModeling
{

    /**
     * @file initial_margin.hpp
     * @brief Dynamic initial margin (DIM): the initial margin of a netting
     *        set on every path at every future date, by regression
     *        (blueprint/wp/23-xva.md §5.6, §10, lot X5).
     *
     * Initial margin covers, at a confidence level, the move of the netting
     * set's value over the margin period of risk (MPoR): what variation
     * margin, called on the value before the default, leaves uncovered.
     * Today it is a number; MVA and the exposure under initial margin need
     * it **in every scenario**.
     *
     * The method is the regression of Anfuso, Aziz, Giltinan & Loukopoulos
     * (2017): on the simulated paths, the squared move of the value over
     * (t, t + MPoR] is regressed on the value at t, which estimates the
     * conditional variance σ²(t, V); the move is taken as Gaussian, so
     *
     *   IM(t) = Φ⁻¹(confidence) × σ(t, V(t)).
     *
     * This is a model of the margin, not the margin rule itself (ISDA SIMM
     * works from sensitivities): `im_today` anchors it, scaling the whole
     * profile so that it starts from the amount actually computed today.
     *
     * Like collateral, it is a post-processing of the cube (ADR-X2): it
     * depends on neither product nor model. The same amount is taken for
     * both parties, as the margin rules for non-cleared derivatives require
     * each to post to the other.
     */

    struct DimSettings
    {
        /// One-sided confidence level of the margin; 99 % is the rule for
        /// non-cleared derivatives.
        Real confidence = 0.99;
        /// The regression of the squared move on the value: one quadratic by
        /// default. A variance has no kink to follow, and buckets would only
        /// add noise.
        RegressionSettings regression{2, 1};
        /// The initial margin actually computed today (a SIMM amount, for
        /// instance): the profile is scaled to start from it. Unset: the
        /// model's own figure.
        std::optional<Real> im_today;
    };

    /// The initial margin of one netting set on the paths of a cube.
    struct InitialMargin
    {
        /// The dates of the collateralised cube: those whose lagged date
        /// t - MPoR is on the grid (collateral_reporting_dates).
        std::vector<Time> times;
        std::size_t paths = 0;
        /// The margin in place today.
        Real today = 0.0;
        /// The factor applied to the model to match DimSettings::im_today.
        Real scaling = 1.0;
        /// margin[p * times.size() + r]: the margin in place at times[r] on
        /// path p — set at times[r] - MPoR, when the last margin call was
        /// honoured.
        std::vector<Real> margin;
        /// E[IM(t)], under the t-forward measure like the exposure profiles.
        std::vector<Real> expected;
        /// E[D(t) IM(t)]: what MVA integrates.
        std::vector<Real> discounted_expected;
    };

    /**
     * @brief The fitted model: for each date, the conditional standard
     *        deviation of the move over the MPoR as a function of the value.
     *
     * Fitted on one cube, it gives the margin on that cube or on any other
     * with the same grid — fresh paths for a backtest, or the scenarios of
     * the historical measure.
     */
    class DynamicInitialMargin
    {
      public:
        /**
         * @param paths  a simulation whose grid carries the lagged dates
         *               (ExposureGridSettings::margin_period_of_risk). With
         *               the trades' cash flows kept, the move is the clean
         *               one: value change plus the flows of the period.
         * @param trades the netted trades; empty means all.
         * @throws InvalidInput on an empty cube, a grid without the lagged
         *         dates, a confidence outside (0.5, 1), or a negative anchor.
         */
        static DynamicInitialMargin fit(const ExposurePaths &paths, Time margin_period_of_risk,
                                        const std::vector<std::size_t> &trades = {},
                                        const DimSettings &settings = {});

        /// The margin on the paths of `paths`, which must have the grid the
        /// model was fitted on.
        InitialMargin margin(const ExposurePaths &paths) const;

        Real today() const { return scaling_ * today_; }
        Real scaling() const { return scaling_; }

        /// The same model with another scaling: the netting set's factor
        /// applied to one of its subsets.
        DynamicInitialMargin scaled(Real scaling) const;

      private:
        std::vector<Time> grid_;
        std::vector<std::size_t> trades_;
        Time mpor_ = 0.0;
        Real quantile_ = 0.0;
        /// Per reporting date: its index on the grid, that of its lagged
        /// date (npos: today), and the variance regression at the lagged date.
        struct Date
        {
            std::size_t index;
            std::size_t lagged;
            BucketedRegression variance;
        };
        std::vector<Date> dates_;
        /// Unscaled margin in place today.
        Real today_ = 0.0;
        Real scaling_ = 1.0;
    };

    /**
     * @brief The initial margin when it is ISDA SIMM, computed on every path
     *        by the simulation (ExposureSimulationSettings::simm): at each
     *        reporting date, the SIMM of the lagged date — the margin that
     *        was in place when the last call was honoured.
     *
     * The margin rule itself, from the sensitivities of each scenario, where
     * DynamicInitialMargin is a model of it. It is the SIMM of **all** the
     * trades of the cube.
     *
     * @throws InvalidInput on a cube simulated without SIMM, or a grid
     *         without the lagged dates.
     */
    InitialMargin simm_initial_margin(const ExposurePaths &paths, Time margin_period_of_risk);

    /// How often the margin was not enough, date by date.
    struct MarginBacktest
    {
        std::vector<Time> times;
        /// Share of the paths on which the value rose by more than the margin
        /// over the MPoR: the bank left uncovered.
        std::vector<Real> uncovered_bank;
        /// The same for a fall: the counterparty left uncovered.
        std::vector<Real> uncovered_counterparty;
        /// Whether the period ending at times[r] is a full MPoR. The first
        /// dates are not (their period starts today): the margin, sized for
        /// a full period, covers them more often than its confidence level.
        std::vector<bool> full_period;
    };

    /**
     * @brief Backtest of a margin on simulated history, as Anfuso et al.
     *        describe: on each path and date, was the realised move over the
     *        MPoR within the margin that was in place?
     *
     * Run it on paths the model was **not** fitted on, and read each
     * frequency against 1 - confidence. Two things the model leaves out show
     * in it: the drift of the value over the period (the margin is centred
     * on no move, so one side is covered slightly more often than the
     * other), and the dates at which the value hardly varies any more — once
     * the last coupon of a swap is fixed its move is all drift, a tiny
     * amount the Gaussian margin does not describe.
     */
    MarginBacktest backtest_initial_margin(const ExposurePaths &paths, const InitialMargin &margin,
                                           Time margin_period_of_risk,
                                           const std::vector<std::size_t> &trades = {});

} // namespace quantModeling

#endif // QM_RISK_INITIAL_MARGIN_HPP
