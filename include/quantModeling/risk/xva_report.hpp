#ifndef QM_RISK_XVA_REPORT_HPP
#define QM_RISK_XVA_REPORT_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/capital.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/initial_margin.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <optional>
#include <vector>

namespace quantModeling
{

    /**
     * @file xva_report.hpp
     * @brief From a simulated cube to the adjustments of one netting set
     *        (blueprint/wp/23-xva.md §7, §9, lot X3): exposure, CVA and DVA
     *        with their Monte-Carlo errors, funding adjustments, the costs of
     *        margin and capital (ColVA, MVA, KVA — lot X5), and the share of
     *        each trade.
     *
     * Everything is read off the same paths: the netting set with and
     * without a trade, with and without the CSA. Signs are Gregory's — a
     * cost is negative (risk/xva.hpp).
     */

    struct XvaInputs
    {
        /// The netted trades, as indices into the cube; empty means all.
        std::vector<std::size_t> trades;
        /// Collateral agreement; none for an uncollateralised netting set.
        /// The cube must then have been simulated with the lagged dates
        /// (ExposureGridSettings::margin_period_of_risk).
        std::optional<Csa> csa;
        CollateralSettings collateral;

        /// Survival curves and losses given default of the counterparty and
        /// of the bank itself.
        CreditCurve counterparty{0.0};
        CreditCurve own{0.0};
        Real lgd_counterparty = 0.6;
        Real lgd_own = 0.6;

        /// The bank's funding spreads over the risk-free rate, to borrow
        /// (FCA) and to lend (FBA).
        Real borrowing_spread = 0.0;
        Real lending_spread = 0.0;

        Real pfe_confidence = 0.95;

        // ── Margin and capital (lot X5) ─────────────────────────────────

        /// Initial margin exchanged both ways, as the margin rules for
        /// non-cleared derivatives require, projected by regression
        /// (risk/initial_margin.hpp). Needs a CSA. The cube should carry
        /// the cash flows (ExposureSimulationSettings::keep_cashflows).
        std::optional<DimSettings> initial_margin;
        /// FS_B - s_IM: the bank's funding spread over what the segregated
        /// margin earns. MVA is zero when it is.
        Real initial_margin_spread = 0.0;
        /// r_c - r: what the CSA pays on cash collateral over the rate the
        /// trades are discounted at. ColVA is zero when it is.
        Real collateral_spread = 0.0;
        /// What the capital projection needs (risk/capital.hpp), its trades
        /// in the order of `trades`; none: no KVA.
        std::optional<CapitalInputs> capital;
        /// CC: the return required on the capital held, typically 10-15 %.
        Real cost_of_capital = 0.10;
    };

    /// A Monte-Carlo estimate with its standard error.
    struct Estimate
    {
        Real value = 0.0;
        Real error = 0.0;
    };

    struct TradeContribution
    {
        std::size_t trade = 0;
        /// CVA of the trade on its own, as if it were not netted.
        Real standalone_cva = 0.0;
        /**
         * @brief Incremental CVA: CVA(netting set) - CVA(netting set without
         *        the trade), under the same CSA. What the trade costs given
         *        what is already there — the price to charge for it. It can
         *        be positive: the trade then reduces the counterparty risk.
         */
        Real incremental_cva = 0.0;
        /**
         * @brief Marginal CVA (Euler allocation): the trade's share of the
         *        netting set's CVA, the shares adding up to it exactly.
         *        NaN under a CSA: with a threshold or a minimum transfer
         *        amount the exposure is no longer homogeneous in the trades
         *        and no exact allocation exists.
         */
        Real marginal_cva = 0.0;
    };

    struct XvaReport
    {
        /// Exposure of the netting set (after collateral when there is a CSA).
        ExposureStatistics exposure;

        /// First-to-default CVA and DVA (each weighted by the survival of
        /// the other party), with their Monte-Carlo errors.
        Estimate cva;
        Estimate dva;
        /// CVA ignoring the bank's own default.
        Real cva_unilateral = 0.0;
        /// Funding cost and benefit; zero with zero spreads. Under initial
        /// margin they are those of the variation margin alone: segregated
        /// margin funds nothing, its cost is the MVA.
        Real fca = 0.0;
        Real fba = 0.0;
        /// Cost (< 0) or gain of the rate paid on the collateral; zero
        /// without a CSA.
        Real colva = 0.0;
        /// Cost of funding the initial margin posted; zero without one.
        Real mva = 0.0;
        /// Cost of the regulatory capital held; zero without capital inputs.
        Real kva = 0.0;

        /// The initial margin, when there is one: in place today, and its
        /// expected profile on the dates of `exposure`.
        Real initial_margin_today = 0.0;
        std::vector<Real> expected_initial_margin;
        /// The capital projection behind the KVA, on the dates of `exposure`.
        std::optional<CapitalProfile> capital;
        /// Gregory's rule of thumb, -spread × EPE × T with the spread
        /// LGD × average hazard: an order of magnitude to set against `cva`.
        Real cva_rule_of_thumb = 0.0;

        /// One row per netted trade.
        std::vector<TradeContribution> contributions;
    };

    /**
     * @brief The report of a netting set.
     * @throws InvalidInput on a cube simulated under the historical measure
     *         (xVA is a price), on a loss given default outside [0, 1], on
     *         initial margin without a CSA, on capital inputs that do not
     *         describe the netted trades, or on whatever exposure_statistics
     *         / collateralise reject.
     */
    XvaReport xva_report(const ExposurePaths &paths, const XvaInputs &inputs);

} // namespace quantModeling

#endif // QM_RISK_XVA_REPORT_HPP
