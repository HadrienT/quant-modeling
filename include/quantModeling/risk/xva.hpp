#ifndef QM_RISK_XVA_HPP
#define QM_RISK_XVA_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/credit_curve.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @file xva.hpp
     * @brief The xVA integrals of Gregory, *The xVA Challenge* (4th ed., Wiley
     *        2020), on exposure profiles that are already computed
     *        (blueprint/wp/23-xva.md §7, §9, §10, §11.7 — lot X0).
     *
     * Every adjustment has the same shape: a projected quantity, integrated
     * against a cost rate, weighted by survival, discounted.
     *
     * **Sign convention — Gregory's, and the cash-flow convention of the
     * front end: an adjustment is negative when it is a cost to the bank.**
     *
     *   V_adjusted = V_risk_free + CVA + DVA + FCA + FBA + MVA + KVA
     *                              <=0   >=0   <=0   >=0   <=0   <=0
     *
     * Many papers (and Green's book) write V - CVA with CVA >= 0 instead.
     *
     * **Discounting.** The profiles are *discounted* expectations, e.g.
     * EE*(t) = E[D(t) max(V(t) - C(t), 0)]: the exposure engine takes the
     * expectation of the discounted quantity, which is not D(t) E[·] when
     * rates are stochastic. No discount curve enters this header.
     *
     * **Independence.** These sums assume that default is independent of the
     * exposure (no wrong-way risk) and that the two defaults are independent
     * of each other. Wrong-way risk is lot X6.
     */

    /**
     * @brief Discounted exposure profile of one netting set, seen from the
     *        bank, at dates t_1 < ... < t_n (t_0 = 0 implicit — the grid
     *        convention of exposure_metrics.hpp).
     */
    struct ExposureProfile
    {
        std::vector<Time> times;
        /// EE*(t_i) >= 0: what the bank loses if the counterparty defaults.
        std::vector<Real> discounted_ee;
        /// ENE*(t_i) <= 0: what the counterparty loses if the bank defaults.
        std::vector<Real> discounted_ene;

        /// EFV*(t_i) = EE*(t_i) + ENE*(t_i).
        std::vector<Real> discounted_efv() const;
        /// The same netting set seen from the counterparty: EE and ENE swap
        /// roles and signs. CVA of one side is DVA of the other.
        ExposureProfile seen_from_counterparty() const;
        /// @throws InvalidInput on mismatched sizes, bad times, a negative EE
        ///         or a positive ENE.
        void validate() const;
    };

    // ── CVA and DVA (blueprint §7) ───────────────────────────────────────────

    /**
     * @brief Unilateral CVA: the bank itself is assumed default-free.
     *
     *   CVA = -LGD_C Σ_i EE*(t_i) PD_C(t_{i-1}, t_i),
     *   PD_C(t_{i-1}, t_i) = S_C(t_{i-1}) - S_C(t_i)
     *
     * @param lgd loss given default of the counterparty, 1 - recovery, in [0, 1].
     * @return a number <= 0.
     */
    Real cva_unilateral(const ExposureProfile &profile, const CreditCurve &counterparty, Real lgd);

    /**
     * @brief Bilateral CVA: only the first default matters, so each term is
     *        weighted by the survival of the bank to the start of the period.
     *
     *   CVA = -LGD_C Σ_i EE*(t_i) S_I(t_{i-1}) PD_C(t_{i-1}, t_i)
     *
     * Dropping the S_I factor double-counts the scenarios where the bank has
     * already defaulted.
     */
    Real cva_bilateral(const ExposureProfile &profile, const CreditCurve &counterparty,
                       const CreditCurve &own, Real lgd_counterparty);

    /**
     * @brief DVA, the mirror image of the bilateral CVA.
     *
     *   DVA = -LGD_I Σ_i ENE*(t_i) S_C(t_{i-1}) PD_I(t_{i-1}, t_i)
     *
     * ENE <= 0, hence DVA >= 0: a gain when the bank's own credit worsens,
     * which is why Basel III removes it from CET1 capital and why it overlaps
     * with FBA (blueprint §7.5). This function only computes it; which
     * components are added up is the caller's documented choice.
     */
    Real dva(const ExposureProfile &profile, const CreditCurve &counterparty,
             const CreditCurve &own, Real lgd_own);

    /**
     * @brief Gregory's "CVA as a spread" rule of thumb (blueprint §7.2): with
     *        a flat hazard rate and negligible discounting,
     *
     *   CVA ≈ -spread × EPE × T
     *
     * so the CVA as a running spread is the EPE times the counterparty's
     * credit spread.
     */
    Real cva_spread_approximation(Real credit_spread, Real epe, Time maturity);

    /// The credit triangle: hazard rate λ ≈ spread / LGD (blueprint §6.2).
    /// @throws InvalidInput if spread < 0 or lgd is not in (0, 1].
    Real hazard_from_spread(Real credit_spread, Real lgd);

    // ── Funding, margin and capital (blueprint §9, §10, §11.7) ───────────────

    /**
     * @brief The sum common to FCA, FBA, MVA and KVA:
     *
     *   -Σ_i X*(t_i) S_C(t_i) S_I(t_i) rate (t_i - t_{i-1})
     *
     * with X* a discounted expected quantity (funding need, initial margin
     * posted, capital held) and `rate` its running cost. The cost stops when
     * either party defaults, hence the joint survival.
     */
    Real running_cost_adjustment(const std::vector<Time> &times,
                                 const std::vector<Real> &discounted_profile,
                                 const CreditCurve &counterparty, const CreditCurve &own,
                                 Real rate);

    /// FCA = -Σ EE*(t_i) S_C S_I FS_B Δt_i <= 0: the cost of funding the
    /// positive exposure at the bank's borrowing spread FS_B.
    Real fca(const ExposureProfile &profile, const CreditCurve &counterparty,
             const CreditCurve &own, Real borrowing_spread);

    /// FBA = -Σ ENE*(t_i) S_C S_I FS_L Δt_i >= 0: the benefit of the funding
    /// the negative exposure provides, at the lending spread FS_L.
    Real fba(const ExposureProfile &profile, const CreditCurve &counterparty,
             const CreditCurve &own, Real lending_spread);

    /// FVA = FCA + FBA. With FS_B = FS_L = FS it collapses to
    /// -FS Σ EFV*(t_i) S_C S_I Δt_i (the symmetric FVA).
    Real fva(const ExposureProfile &profile, const CreditCurve &counterparty,
             const CreditCurve &own, Real borrowing_spread, Real lending_spread);

    /**
     * @brief ColVA = -Σ E[D C](t_i) S_C S_I (r_c - r) Δt_i: the cost (< 0) of
     *        paying on the collateral held a rate r_c above the rate r the
     *        trades are discounted at — or, on collateral posted, of earning
     *        less than r.
     *
     * @param discounted_expected_collateral > 0 held by the bank, < 0 posted.
     * @param collateral_spread r_c - r; zero for a CSA that pays the
     *        overnight rate, which is why collateralised trades are
     *        discounted on the OIS curve (Piterbarg 2010).
     */
    Real colva(const std::vector<Time> &times, const std::vector<Real> &discounted_expected_collateral,
               const CreditCurve &counterparty, const CreditCurve &own, Real collateral_spread);

    /**
     * @brief MVA = -Σ E[D IM_I](t_i) S_C S_I (FS_B - s_IM) Δt_i <= 0.
     *
     * @param discounted_expected_im   the initial margin the bank **posts**
     *                                 (segregated: it funds nothing), >= 0.
     * @param funding_spread_over_im_return FS_B minus the return s_IM earned
     *                                 on the segregated margin.
     */
    Real mva(const std::vector<Time> &times, const std::vector<Real> &discounted_expected_im,
             const CreditCurve &counterparty, const CreditCurve &own,
             Real funding_spread_over_im_return);

    /**
     * @brief KVA = -Σ E[D K](t_i) S_C S_I CC Δt_i <= 0.
     *
     * @param discounted_expected_capital projected regulatory capital, >= 0.
     * @param cost_of_capital the return shareholders require, typically 10-15 %.
     */
    Real kva(const std::vector<Time> &times, const std::vector<Real> &discounted_expected_capital,
             const CreditCurve &counterparty, const CreditCurve &own, Real cost_of_capital);

} // namespace quantModeling

#endif // QM_RISK_XVA_HPP
