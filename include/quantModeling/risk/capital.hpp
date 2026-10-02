#ifndef QM_RISK_CAPITAL_HPP
#define QM_RISK_CAPITAL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/risk/initial_margin.hpp"
#include "quantModeling/risk/regulatory/ba_cva.hpp"
#include "quantModeling/risk/regulatory/sa_ccr.hpp"

#include <optional>
#include <vector>

namespace quantModeling
{

    /**
     * @file capital.hpp
     * @brief The regulatory capital a netting set will tie up over its life,
     *        projected on the simulated paths — what KVA charges for
     *        (blueprint/wp/23-xva.md §11.7, lot X5; Green, Kenyon & Dennis
     *        2014).
     *
     * Two charges, both proportional to the SA-CCR exposure at default:
     *
     *   K(t) = K_IRB(PD, LGD, M(t)) EAD(t)          default risk (CRE31)
     *        + DS (RW / alpha) M(t) DF(M(t)) EAD(t)  CVA risk, reduced BA-CVA (MAR50)
     *
     * At a future date t the SA-CCR add-on depends on the trades that are
     * left — their remaining maturities, the same on every path — while the
     * replacement cost and the multiplier depend on the path's value. So
     * EAD(t) is computed path by path from the cube, with add-ons computed
     * once per date on the **aged** trades.
     *
     * Approximations, all on the supervisory side: an option keeps the
     * moneyness it has today in its supervisory delta, and once its last
     * exercise date has passed it is carried as its underlying swap, as if
     * exercised. The market-risk capital of the hedges and the leverage
     * ratio are left out.
     */

    struct CapitalInputs
    {
        /// The SA-CCR description of each netted trade **as of today**.
        std::vector<sa_ccr::Trade> trades;
        /// The margin agreement, when the counterparty posts variation
        /// margin; its NICA is the independent amount (the initial margin of
        /// a path is added to it).
        std::optional<sa_ccr::MarginAgreement> margin;

        /// One-year probability of default of the counterparty, before the
        /// floor of 0.05 % (CRE32.4).
        Real pd = 0.0;
        /// Regulatory loss given default: 40 % for a senior unsecured claim
        /// on a corporate under the foundation approach, 45 % on a financial
        /// institution (CRE32.6).
        Real lgd = 0.40;
        /// Asset correlation × 1.25 (CRE31.7).
        bool large_financial = false;

        /// The counterparty's row and column in the BA-CVA risk weights
        /// (MAR50.16).
        ba_cva::Sector sector = ba_cva::Sector::Other;
        ba_cva::CreditQuality quality = ba_cva::CreditQuality::HighYieldOrNotRated;
    };

    struct CapitalProfile
    {
        std::vector<Time> times;
        /// Today's figures.
        Real ead_today = 0.0;
        Real default_capital_today = 0.0;
        Real cva_capital_today = 0.0;
        /// E[EAD(t)], under the t-forward measure like the exposure profiles.
        std::vector<Real> expected_ead;
        /// E[D(t) K(t)], by charge and in total: what KVA integrates.
        std::vector<Real> discounted_default_capital;
        std::vector<Real> discounted_cva_capital;
        std::vector<Real> discounted_capital;
    };

    /// The PD floor of CRE32.4.
    inline constexpr Real irb_pd_floor = 0.0005;

    /**
     * @brief A trade as SA-CCR sees it `t` years from now: maturities and the
     *        option's exercise date shortened by t. Empty once the trade has
     *        matured. An option past its last exercise date is its
     *        underlying, in the direction the option gave.
     */
    std::optional<sa_ccr::Trade> aged_trade(const sa_ccr::Trade &trade, Time t);

    /**
     * @brief The effective maturity of the netting set at t: the
     *        notional-weighted remaining maturity of its trades (CRE32.49),
     *        floored at one year (CRE32.46). The five-year cap of CRE32.46
     *        applies to the default-risk charge, not to BA-CVA (MAR50.15):
     *        the caller applies it. Zero when no trade is left.
     */
    Time effective_maturity(const std::vector<sa_ccr::Trade> &trades, Time t);

    /**
     * @brief Projects the capital of one netting set.
     *
     * @param netting_set a cube holding one "trade", the value of the netting
     *        set net of the variation margin held (V - C): the trades netted
     *        when there is no CSA, collateralise() without initial margin
     *        when there is one.
     * @param margin_received the initial margin held on each path, when
     *        there is one: it counts as collateral (it lowers V - C) and as
     *        NICA.
     * @throws InvalidInput on a cube with several trades or under the
     *         historical measure, a PD outside [0, 1), an LGD outside [0, 1],
     *         or whatever SA-CCR rejects in the trades.
     */
    CapitalProfile projected_capital(const ExposurePaths &netting_set, const CapitalInputs &inputs,
                                     const InitialMargin *margin_received = nullptr);

} // namespace quantModeling

#endif // QM_RISK_CAPITAL_HPP
