#ifndef QM_MARKET_CSA_HPP
#define QM_MARKET_CSA_HPP

#include "quantModeling/core/types.hpp"

#include <limits>

namespace quantModeling
{

    /**
     * @brief The terms of a Credit Support Annex that drive exposure
     *        (blueprint/wp/23-xva.md §4.3, lot X2). Plain data: the
     *        collateral mechanics are in risk/collateral.hpp.
     *
     * The variation margin a perfect, instantaneous CSA would hold for a
     * netting set worth V to the bank is
     *
     *   C(V) = max(V - H_C, 0) - max(-V - H_I, 0)
     *
     * positive when the bank holds collateral, negative when it has posted
     * some. Real CSAs depart from it through the minimum transfer amount, the
     * rounding and, above all, the margin period of risk.
     */
    struct Csa
    {
        /// H_C: the counterparty posts nothing while the bank's exposure is
        /// below it. Infinite: the counterparty never posts.
        Real threshold_counterparty = 0.0;
        /// H_I: the same for the bank. Infinite: a one-way CSA in the bank's
        /// favour.
        Real threshold_bank = 0.0;
        /// MTA: no transfer below this amount.
        Real minimum_transfer_amount = 0.0;
        /// Transfers are rounded to a multiple of this amount; 0 for none.
        Real rounding = 0.0;
        /// IA: collateral held by the bank on top of the variation margin
        /// (< 0 if the bank posted it), whatever the value of the trades. Not
        /// segregated: it nets against the exposure.
        Real independent_amount = 0.0;
        /// MPoR in years: the collateral available at a default at t is the
        /// one called on the value at t - MPoR. Ten business days is the
        /// bilateral standard (CRE52.50); zero is the instantaneous CSA.
        Time margin_period_of_risk = 10.0 / 250.0;

        /// No collateral at all: both thresholds infinite.
        static Csa uncollateralised()
        {
            Csa csa;
            csa.threshold_counterparty = std::numeric_limits<Real>::infinity();
            csa.threshold_bank = std::numeric_limits<Real>::infinity();
            return csa;
        }

        /// @throws InvalidInput on a negative threshold, MTA, rounding or MPoR.
        void validate() const
        {
            if (!(threshold_counterparty >= 0.0) || !(threshold_bank >= 0.0))
                throw InvalidInput("CSA: thresholds must be >= 0");
            if (!(minimum_transfer_amount >= 0.0) || !(rounding >= 0.0))
                throw InvalidInput("CSA: minimum transfer amount and rounding must be >= 0");
            if (!(margin_period_of_risk >= 0.0))
                throw InvalidInput("CSA: the margin period of risk must be >= 0");
        }
    };

} // namespace quantModeling

#endif // QM_MARKET_CSA_HPP
