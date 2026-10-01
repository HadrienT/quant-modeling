#ifndef QM_RISK_EXPOSURE_METRICS_HPP
#define QM_RISK_EXPOSURE_METRICS_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @file exposure_metrics.hpp
     * @brief The exposure metrics of Gregory, *The xVA Challenge* (4th ed.,
     *        Wiley 2020), with his names and his sign convention
     *        (blueprint/wp/23-xva.md §3, lot X0).
     *
     * With V(t) the value of the netting set seen from the bank and C(t) the
     * collateral it holds:
     *
     *   EFV(t)   = E[V(t) - C(t)]             expected future value
     *   EE(t)    = E[max(V(t) - C(t), 0)]     expected exposure          (>= 0)
     *   ENE(t)   = E[min(V(t) - C(t), 0)]     expected negative exposure (<= 0)
     *   PFE_a(t) = quantile a of V(t) - C(t)  potential future exposure
     *   EPE      = (1/T) ∫_0^T EE(t) dt       expected positive exposure
     *   EEE(t_k) = max(EEE(t_{k-1}), EE(t_k)) effective EE (non-decreasing)
     *   EEPE     = time average of EEE over the first year
     *
     * so that EE + ENE = EFV. ENE is a **negative** number, as in Gregory:
     * that is what makes DVA and FBA come out positive (a benefit) from the
     * same formula that makes CVA and FCA negative (a cost).
     *
     * This header only holds what needs no simulation: the closed forms under
     * a normal distribution — the oracles the exposure engine of lot X1 has to
     * reproduce — and the functionals of a profile that is already computed.
     *
     * **Grid convention**, shared with risk/xva.hpp: a profile is given at
     * dates t_1 < ... < t_n, with t_0 = 0 implicit. Time integrals are the
     * sums Σ_i f(t_i) (t_i - t_{i-1}) of Gregory's discrete formulas and of
     * the Basel definition of the EEPE (CRE53.13). A first date t_1 = 0 is
     * accepted and contributes nothing.
     */

    // ── Closed forms, V ~ N(mu, sigma²) (blueprint §3.2) ────────────────────

    /// EE = mu Φ(mu/sigma) + sigma φ(mu/sigma). For mu = 0 this is
    /// sigma φ(0) ≈ 0.40 sigma — the "0.4" of Gregory's rules of thumb.
    /// sigma = 0 gives max(mu, 0).
    /// @throws InvalidInput if sigma < 0 or an argument is not finite.
    Real normal_expected_exposure(Real mu, Real sigma);

    /// ENE = mu Φ(-mu/sigma) - sigma φ(mu/sigma), a number <= 0.
    Real normal_expected_negative_exposure(Real mu, Real sigma);

    /// PFE_a = mu + sigma Φ⁻¹(a): the quantile of the *value*, which is the
    /// quantile of the exposure max(V, 0) as soon as it is positive.
    /// @throws InvalidInput unless 0 < confidence < 1.
    Real normal_potential_future_exposure(Real mu, Real sigma, Real confidence);

    /**
     * @brief Gregory's netting factor: netted exposure / gross exposure for
     *        n centred normal exposures of equal variance and average pairwise
     *        correlation rho (blueprint §4.1).
     *
     *   factor = sqrt(n + n (n - 1) rho) / n
     *
     * 1/sqrt(n) for independent trades, 1 for perfectly correlated ones:
     * netting pays on a diversified book, not on a directional one.
     *
     * @throws InvalidInput if n == 0 or rho is outside [-1/(n-1), 1] (below
     *         that bound the correlation matrix is not positive semi-definite).
     */
    Real netting_factor(std::size_t n, Real average_correlation);

    // ── Functionals of a profile ─────────────────────────────────────────────

    /// EPE = (1/t_n) Σ_i EE(t_i) (t_i - t_{i-1}).
    /// @throws InvalidInput on an empty or mismatched profile, or on times
    ///         that are negative or not strictly increasing.
    Real expected_positive_exposure(const std::vector<Time> &times, const std::vector<Real> &ee);

    /// Effective EE: the running maximum of the EE profile. Regulatory
    /// capital assumes that trades which mature are replaced, so the exposure
    /// is not allowed to roll off (CRE53.12). The recursion of CRE53.12
    /// starts from the current exposure: put t = 0 on the grid to include it.
    std::vector<Real> effective_expected_exposure(const std::vector<Real> &ee);

    /**
     * @brief Effective EPE: the time average of the Effective EE over the
     *        first year, or over the life of the netting set when it is
     *        shorter (CRE53.13). EAD under the IMM is alpha × EEPE.
     *
     * The Effective EE is held flat between grid dates, so a grid that
     * straddles the horizon is cut at the horizon exactly.
     *
     * @param horizon one year in the regulation; a parameter so that the
     *                property "EEPE over the whole life >= EPE" can be tested.
     */
    Real effective_expected_positive_exposure(const std::vector<Time> &times,
                                              const std::vector<Real> &ee, Time horizon = 1.0);

} // namespace quantModeling

#endif // QM_RISK_EXPOSURE_METRICS_HPP
