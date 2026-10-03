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
        /// More quantiles of the netting set's value to report with the
        /// exposure (ExposureStatistics::value_quantiles).
        std::vector<Real> quantile_levels;

        /**
         * @brief Wrong-way risk (lot X6, risk/wrong_way_risk.hpp): the
         *        counterparty's hazard is exp(a(t) + b V(t)), V the value of
         *        the netted trades before collateral. Per unit of currency;
         *        0 is independence, > 0 wrong-way, < 0 right-way.
         *
         * It changes CVA and DVA (the counterparty's survival weighs the
         * bank's own default). The funding, margin and capital adjustments
         * keep the market survival curve.
         */
        Real wrong_way_b = 0.0;

        // ── Margin and capital (lot X5) ─────────────────────────────────

        /// Initial margin exchanged both ways, as the margin rules for
        /// non-cleared derivatives require, projected by regression
        /// (risk/initial_margin.hpp). Needs a CSA. The cube should carry
        /// the cash flows (ExposureSimulationSettings::keep_cashflows).
        std::optional<DimSettings> initial_margin;
        /**
         * @brief Where the initial margin comes from (lot X5b). With either
         *        SIMM choice the cube must have been simulated with
         *        ExposureSimulationSettings::simm.
         */
        enum class MarginModel
        {
            /// The regression's own figure (or DimSettings::im_today).
            Regression,
            /// The regression's profile, scaled to start from today's SIMM:
            /// what the work package calls "calibrated on a SIMM today".
            RegressionOnSimm,
            /// SIMM itself, from the sensitivities of each path. For the
            /// netting set of all the cube's trades; the shares of each
            /// trade use the scaled regression.
            SimmPerPath
        };
        MarginModel margin_model = MarginModel::Regression;
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
         *        and no exact allocation exists. NaN under wrong-way risk
         *        too: the hazard depends on the value of the whole set.
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
        /// The same two with the counterparty's default independent of the
        /// exposure: equal to `cva` and `dva` unless XvaInputs::wrong_way_b
        /// is set. cva / cva_independent is the cost of the wrong-way risk.
        Estimate cva_independent;
        Estimate dva_independent;
        /// CVA ignoring the bank's own default (and any wrong-way risk).
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

        /**
         * @brief Monte-Carlo standard errors of the adjustments above.
         *
         * FCA, FBA, their sum, ColVA and MVA are averages over the paths of
         * a sum over the dates: the dispersion of the per-path sums gives
         * the error, correlations between dates included (and between FCA
         * and FBA, for `fva_error`). The MVA's is conditional on the margin
         * model: it does not carry the noise of the regression that fitted
         * it. The KVA's is by batch means (error_batches()): the capital of
         * the internal models method is a function of the whole expected
         * profile, not an average over paths.
         */
        Real fca_error = 0.0;
        Real fba_error = 0.0;
        Real fva_error = 0.0;
        Real colva_error = 0.0;
        Real mva_error = 0.0;
        Real kva_error = 0.0;

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
     * @brief Probability that `defaulter` defaults in (t_{i-1}, t_i] with
     *        `other` still alive at t_{i-1}, per date:
     *        S_other(t_{i-1}) (S_defaulter(t_{i-1}) - S_defaulter(t_i)).
     *
     * The weights of the first-to-default sums: -LGD times them, applied to
     * the discounted positive exposure of a path, is the CVA of that path.
     */
    std::vector<Real> first_to_default_weights(const std::vector<Time> &times,
                                               const CreditCurve &defaulter, const CreditCurve &other);

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
