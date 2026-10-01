#ifndef QM_RISK_EXPOSURE_PATHS_HPP
#define QM_RISK_EXPOSURE_PATHS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/risk/xva.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @brief The output of an exposure simulation: the value of every trade
     *        on every path at every date (blueprint/wp/23-xva.md §13.3).
     *
     * It carries no product and no model: netting, collateral, exposure
     * metrics and xVA are post-processing of this cube. Keeping the trades
     * apart is what lets one simulation answer for any netting set, for the
     * incremental exposure of a new trade and for the Euler allocation, all
     * on the **same paths**, so that differences are not drowned in
     * Monte-Carlo noise.
     *
     * Matrices are stored row by row: element (path p, date i) is at
     * `p * dates() + i`.
     */
    struct ExposurePaths
    {
        /// t_1 < ... < t_n; today is not on the grid.
        std::vector<Time> times;
        /// P(0, t_i), today's discount factors.
        std::vector<Real> discount;
        std::size_t paths = 0;

        /**
         * @brief Stochastic discount weight w(p, i): for any payoff X known
         *        at t_i,
         *
         *   E^Q[D(t_i) X(t_i)] ≈ (1/paths) Σ_p w(p, i) X(p, i).
         *
         * It is the discount factor D(t_i) of the path when the simulation
         * runs under the risk-neutral measure, and P(0, T*) / P(t_i, T*) when
         * it runs under the T*-forward measure, as the Hull-White engine
         * does. Its mean is P(0, t_i).
         */
        std::vector<Real> discount_weight;

        /// Value today of each trade, times its quantity.
        std::vector<Real> trade_values_today;
        /// Per trade: value at (path, date) of the cash flows strictly after
        /// that date, times the quantity.
        std::vector<std::vector<Real>> trade_values;
        /// Per trade: cash flow at (path, date), > 0 received. Empty unless
        /// the simulation was asked to keep them.
        std::vector<std::vector<Real>> trade_cashflows;

        std::size_t dates() const { return times.size(); }
        std::size_t trades() const { return trade_values.size(); }
    };

    /**
     * @brief Exposure metrics of one netting set, with Gregory's names and
     *        signs (risk/exposure_metrics.hpp).
     *
     * Two families of profiles:
     *  - **discounted**, EE*(t) = E^Q[D(t) max(V(t), 0)]: what CVA, DVA and
     *    FVA integrate (risk/xva.hpp);
     *  - **undiscounted**, EE(t) = EE*(t) / P(0, t): the expectation under
     *    the t-forward measure, what limits (PFE) and regulatory capital
     *    (EEPE) are stated in.
     */
    struct ExposureStatistics
    {
        std::vector<Time> times;
        /// The trades that were netted, as indices into ExposurePaths.
        std::vector<std::size_t> trades;
        Real value_today = 0.0;

        std::vector<Real> discounted_ee;
        std::vector<Real> discounted_ene;
        std::vector<Real> discounted_efv;
        /// Monte-Carlo standard errors of the three profiles above.
        std::vector<Real> discounted_ee_error;
        std::vector<Real> discounted_ene_error;
        std::vector<Real> discounted_efv_error;

        std::vector<Real> ee;
        std::vector<Real> ene;
        std::vector<Real> efv;

        /// PFE: max(quantile of V(t) under the t-forward measure, 0).
        std::vector<Real> pfe;
        Real pfe_confidence = 0.95;

        /// EPE and Effective EPE of the undiscounted EE profile.
        Real epe = 0.0;
        Real eepe = 0.0;

        /**
         * @brief Euler allocation of EE* to the trades (blueprint §4.1):
         *        contribution of trade k at t_i = E[D V_k 1{V_NS > 0}].
         *
         * Read off the same paths as the total, to which the contributions
         * add up exactly. Row j is the trade `trades[j]`. A contribution can
         * be negative: the trade then reduces the exposure of the netting set.
         */
        std::vector<std::vector<Real>> discounted_ee_contributions;

        /// The profile that risk/xva.hpp integrates.
        ExposureProfile profile() const;
    };

    /**
     * @brief Nets the given trades path by path and computes the metrics.
     *
     * Close-out netting is the sum inside the positive part,
     * max(Σ_k V_k, 0) <= Σ_k max(V_k, 0): to measure what netting is worth,
     * call this once per trade and once for the set.
     *
     * The reductions run over the paths in index order, on one thread: the
     * result does not depend on how the simulation was parallelised.
     *
     * @param trades         indices of the netted trades; empty means all.
     * @param pfe_confidence quantile of the PFE, in (0, 1).
     * @throws InvalidInput on an empty simulation, an index out of range or
     *         a repeated index.
     */
    ExposureStatistics exposure_statistics(const ExposurePaths &paths,
                                           const std::vector<std::size_t> &trades = {},
                                           Real pfe_confidence = 0.95);

} // namespace quantModeling

#endif // QM_RISK_EXPOSURE_PATHS_HPP
