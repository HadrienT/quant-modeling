#ifndef QM_RISK_REGULATORY_IRB_HPP
#define QM_RISK_REGULATORY_IRB_HPP

#include "quantModeling/core/types.hpp"

namespace quantModeling::irb
{

    /**
     * @file irb.hpp
     * @brief The IRB risk-weight function for corporate, sovereign and bank
     *        exposures (Basel framework CRE31.5-7) — blueprint/wp/23-xva.md
     *        §11.3, lot X0. It turns the EAD of a netting set into the
     *        default-risk capital that the KVA charges for.
     *
     *   K = LGD [Φ((Φ⁻¹(PD) + sqrt(R) Φ⁻¹(0.999)) / sqrt(1 - R)) - PD]
     *       × (1 + (M - 2.5) b) / (1 - 1.5 b)
     *
     * The bracket is the loss at the 99.9 % quantile of a one-factor Vasicek
     * model (the "ASRF" model) minus the expected loss, which provisions
     * already cover; the last factor is the maturity adjustment.
     *
     * The formula is published as an image; the coefficients here are
     * checked in tests/testRegulatoryCapital.cpp against the illustrative
     * risk weights of CRE99 Table 1.
     */

    /// Quantile of the systematic factor (CRE31.5).
    inline constexpr Real confidence = 0.999;
    /// RWA = 12.5 × K × EAD: the inverse of the 8 % minimum capital ratio.
    inline constexpr Real rwa_multiplier = 12.5;

    /**
     * @brief Asset correlation R, from 24 % (PD -> 0) down to 12 % (PD -> 1).
     *
     *   R = 0.12 w + 0.24 (1 - w),   w = (1 - exp(-50 PD)) / (1 - exp(-50))
     *
     * @param large_financial × 1.25 for regulated financial institutions with
     *        total assets >= USD 100 bn and for unregulated ones (CRE31.7).
     * @throws InvalidInput unless 0 < pd < 1.
     */
    Real asset_correlation(Real pd, bool large_financial = false);

    /// Maturity adjustment b = (0.11852 - 0.05478 ln PD)².
    Real maturity_adjustment(Real pd);

    /**
     * @brief Capital requirement K per unit of EAD.
     *
     * @param pd       one-year probability of default, in (0, 1). The
     *                 regulatory floor on PD (CRE32) is the caller's business.
     * @param lgd      loss given default, in [0, 1].
     * @param maturity effective maturity M in years (2.5 is the reference at
     *                 which the adjustment is 1 / (1 - 1.5 b)).
     */
    Real capital_requirement(Real pd, Real lgd, Time maturity, bool large_financial = false);

    /// RWA = 12.5 × K × EAD.
    Real risk_weighted_assets(Real capital_requirement, Real ead);

} // namespace quantModeling::irb

#endif // QM_RISK_REGULATORY_IRB_HPP
