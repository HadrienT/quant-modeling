#ifndef QM_RISK_REGULATORY_IM_SCHEDULE_HPP
#define QM_RISK_REGULATORY_IM_SCHEDULE_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling::im_schedule
{

    /**
     * @file im_schedule.hpp
     * @brief The standardised initial margin schedule for non-centrally
     *        cleared derivatives (BCBS-IOSCO, *Margin requirements for
     *        non-centrally cleared derivatives*, April 2020, paragraph 3.6 and
     *        Appendix A) — blueprint/wp/23-xva.md §5.2, lot X0.
     *
     * The alternative to a model such as ISDA SIMM: a percentage of the
     * notional per trade, then a partial recognition of netting through the
     * net-to-gross ratio. Simple and very conservative, which is what pushed
     * the market towards SIMM.
     */

    enum class AssetClass
    {
        Credit,
        Commodity,
        Equity,
        ForeignExchange,
        InterestRate,
        Other
    };

    struct Trade
    {
        AssetClass asset_class = AssetClass::Other;
        Real notional = 0.0;
        /// Remaining duration in years; only credit and interest rate use it.
        Time duration = 0.0;
        /// Current market value, for the net-to-gross ratio.
        Real market_value = 0.0;
    };

    /**
     * @brief Appendix A, as a fraction of the notional:
     *        credit 2 % / 5 % / 10 % and interest rate 1 % / 2 % / 4 % for a
     *        duration of 0-2, 2-5 and 5+ years; FX 6 %; commodity, equity and
     *        other 15 %.
     *
     * The text does not say where exactly 2 and 5 years fall; they are put in
     * the lower bucket here.
     */
    Real margin_rate(AssetClass asset_class, Time duration);

    /// Gross IM = Σ_k notional_k × rate_k.
    Real gross_initial_margin(const std::vector<Trade> &trades);

    /**
     * @brief NGR = net replacement cost / gross replacement cost
     *            = max(Σ_k V_k, 0) / Σ_k max(V_k, 0).
     *
     * When no trade has a positive value the ratio is 0/0 and the text is
     * silent: 1 is returned, i.e. no netting benefit — the conservative
     * choice.
     */
    Real net_to_gross_ratio(const std::vector<Trade> &trades);

    /// Net IM = 0.4 Gross IM + 0.6 NGR Gross IM: between 40 % and 100 % of
    /// the gross amount.
    Real net_initial_margin(Real gross_initial_margin, Real net_to_gross_ratio);
    Real net_initial_margin(const std::vector<Trade> &trades);

} // namespace quantModeling::im_schedule

#endif // QM_RISK_REGULATORY_IM_SCHEDULE_HPP
