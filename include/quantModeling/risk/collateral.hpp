#ifndef QM_RISK_COLLATERAL_HPP
#define QM_RISK_COLLATERAL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/collateral_path.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @file collateral.hpp
     * @brief Collateral as a post-processing of the simulated values
     *        (blueprint/wp/23-xva.md §4.3-4.4, §5.5, ADR-X2 — lot X2).
     *
     * Collateral depends on neither product nor model: it is a function of
     * the value of the netting set path by path. Running it on the cube of an
     * uncollateralised simulation is what makes "with and without CSA"
     * comparable on the same paths.
     *
     * The model is Gregory's classical one: if the counterparty defaults at
     * t, the collateral in hand is the one that was called on the value at
     * t - MPoR, because the last calls went unanswered:
     *
     *   E(t) = max(V(t) - C(t - MPoR), 0)
     *
     * Even with a zero threshold and daily calls the exposure is therefore
     * not zero: it is the move of the value over the margin period of risk,
     * about 0.4 σ_V sqrt(MPoR) for a Gaussian value.
     */

    struct CollateralSettings
    {
        MarginPeriodCashflows cashflows = MarginPeriodCashflows::Paid;
        /// Initial margin **received** by the bank, one value per date of the
        /// collateralised cube (the reporting dates), or empty for none.
        /// It only reduces the bank's exposure (blueprint §5.5).
        std::vector<Real> initial_margin_received;
        /// Initial margin **posted** by the bank, segregated: it only reduces
        /// the counterparty's exposure to the bank.
        std::vector<Real> initial_margin_posted;
        /// The same two margins when they depend on the path (a dynamic
        /// initial margin, risk/initial_margin.hpp): paths × reporting dates,
        /// row by row. A non-empty matrix takes precedence over the profile.
        std::vector<Real> initial_margin_received_paths;
        std::vector<Real> initial_margin_posted_paths;
    };

    /// The variation margin of a perfect CSA for a value V:
    /// max(V - H_C, 0) - max(-V - H_I, 0).
    Real required_variation_margin(Real value, const Csa &csa);

    /**
     * @brief One margin call: the balance after it, given the balance before
     *        and the amount the CSA requires.
     *
     * Nothing moves while |required - held| is below the minimum transfer
     * amount; otherwise the transfer is rounded to the nearest multiple of
     * the CSA's rounding. (A real CSA rounds deliveries up and returns down;
     * the difference is at most one rounding unit.)
     */
    Real collateral_after_call(Real held, Real required, const Csa &csa);

    /// The dates of `paths` at which collateralised exposure can be computed
    /// for this MPoR: those whose lagged date t - MPoR is on the grid, or is
    /// today or earlier. The engine puts the lagged dates on the grid when
    /// ExposureGridSettings::margin_period_of_risk is set.
    std::vector<std::size_t> collateral_reporting_dates(const ExposurePaths &paths,
                                                        Time margin_period_of_risk);

    /**
     * @brief The dates of a CSA on a grid, and the balance held today: what
     *        the per-path functions of risk/collateral_path.hpp run on.
     *
     * @param value_today the value today of the netted trades.
     * @throws InvalidInput if the last date of the grid has no lagged date
     *         t - MPoR on it: the grid was built without the lagged dates.
     */
    CollateralPlan collateral_plan(const std::vector<Time> &times, const Csa &csa, Real value_today,
                                   MarginPeriodCashflows cashflows = MarginPeriodCashflows::Paid);

    /// A margin period of risk on the grid: a reporting date and its lagged
    /// date t - MPoR.
    struct MarginPeriod
    {
        /// The lagged date is today (or earlier).
        static constexpr std::size_t kToday = static_cast<std::size_t>(-1);
        std::size_t date;
        std::size_t lagged;
    };

    /// The reporting dates of collateral_reporting_dates(), each with its
    /// lagged date.
    std::vector<MarginPeriod> margin_periods(const std::vector<Time> &times,
                                             Time margin_period_of_risk);

    /**
     * @brief Applies a CSA to a netting set and returns a cube holding one
     *        "trade": the collateralised value V(t) - C(t - MPoR), on the
     *        reporting dates only. Feed it to exposure_statistics().
     *
     * The collateral balance of each path starts from the call on today's
     * value and is updated at every lagged date in time order — the minimum
     * transfer amount makes it path-dependent. The independent amount is
     * added on top.
     *
     * With initial margin the value is cut on both sides: above IM received
     * for the bank's exposure, below -IM posted for the counterparty's, zero
     * in between. Its positive and negative parts are then the two exposures;
     * its mean (EFV) no longer is a funding requirement — compute FVA on the
     * cube without initial margin.
     *
     * The Euler allocation is not carried over: with a threshold or an MTA
     * the exposure is no longer homogeneous in the trades (blueprint §4.1).
     *
     * @param trades the netted trades; empty means all.
     * @throws InvalidInput if the grid was built without the lagged dates, if the
     *         cash-flow treatment needs flows the simulation did not keep, or
     *         if an initial-margin profile has the wrong length or sign.
     */
    ExposurePaths collateralise(const ExposurePaths &paths, const Csa &csa,
                                const std::vector<std::size_t> &trades = {},
                                const CollateralSettings &settings = {});

    /**
     * @brief E[D(t) C(t - MPoR)] on the reporting dates: the discounted
     *        expected collateral the bank holds (> 0) or has posted (< 0),
     *        variation margin and independent amount. What ColVA integrates
     *        (risk/xva.hpp).
     */
    std::vector<Real> discounted_expected_collateral(const ExposurePaths &paths, const Csa &csa,
                                                     const std::vector<std::size_t> &trades = {});

    /// The same before the average: element (path p, reporting date r) is at
    /// `p * dates + r`. What the Monte-Carlo error of ColVA is read on.
    std::vector<Real> discounted_collateral_paths(const ExposurePaths &paths, const Csa &csa,
                                                  const std::vector<std::size_t> &trades = {});

    /**
     * @brief The simplest projection of initial margin (blueprint §5.6):
     *        today's amount, decaying like the square root of the remaining
     *        life, im_today × sqrt(max(maturity - t, 0) / maturity).
     *
     * Biased, and everybody's starting point; the regression of
     * risk/initial_margin.hpp replaces it.
     */
    std::vector<Real> deterministic_initial_margin(Real im_today, const std::vector<Time> &times,
                                                   Time maturity);

} // namespace quantModeling

#endif // QM_RISK_COLLATERAL_HPP
