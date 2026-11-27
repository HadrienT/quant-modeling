#ifndef ENGINE_ANALYTIC_CURVE_CLOSED_FORMS_HPP
#define ENGINE_ANALYTIC_CURVE_CLOSED_FORMS_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/forward_curve.hpp"

#include <string>
#include <utility>
#include <vector>

namespace quantModeling
{
    /**
     * @brief Black-Scholes closed forms on a ForwardCurve (WP 32 F6,
     *        blueprint/wp/32-forward-curve.md).
     *
     * Under Black-Scholes on a curve, ln S(t) is Gaussian with mean
     * ln F(t) − σ²t/2 and Cov(ln S(s), ln S(t)) = σ² min(s, t), whatever the
     * carry between the dates: a closed form reads the curve only through
     * the forwards at its dates and the discount at its payment. Each one
     * returns its price and its sensitivities to those log forwards and log
     * discounts, from which curve_pillar_risks() gives the risk to every
     * pillar exactly, without a bump.
     *
     * The analytic engines of the catalog (BSEuroVanillaAnalyticEngine,
     * the Asian engines, ...) keep their scalar (r, q): their inputs are
     * typed, and a flat curve is its scalars.
     */
    struct CurveSensitivity
    {
        /// (t, ∂V/∂ln F(t)): one entry per date the price reads a forward at.
        std::vector<std::pair<Time, Real>> log_forward;
        /// (t, ∂V/∂ln DF(t)): one entry per payment date.
        std::vector<std::pair<Time, Real>> log_discount;
    };

    struct CurveClosedForm
    {
        Real price = 0.0;
        CurveSensitivity sensitivity;
    };

    /// A pillar's risk, labelled as the Monte-Carlo's (CurveCarry):
    /// "zero[t]" the zero rate of a discount pillar (the forwards moving
    /// with it, as the scalar rate), "div_yield[t]" the dividend yield of a
    /// forward pillar.
    struct PillarRisk
    {
        std::string label;
        Real value = 0.0;
    };

    /// The risks per pillar of a price whose curve sensitivities are `s`:
    /// Δ ln DF(t) = −Σ_j w_j(t) t_j δ_j, Δ ln F(t) = Σ_j w_j(t) t_j δ_j −
    /// Σ_i u_i(t) t_i ε_i, through the curve's own interpolation weights.
    /// They sum to the parallel rho and dividend risk. Every pillar is
    /// listed, discount pillars first, a pillar the price does not read at
    /// zero; empty for a flat curve.
    std::vector<PillarRisk> curve_pillar_risks(const ForwardCurve &curve, const CurveSensitivity &s);

    /// The parallel shifts' risks, ∂V/∂r and ∂V/∂q: the sums of the pillars'.
    std::pair<Real, Real> curve_parallel_risks(const CurveSensitivity &s);

    /// A call or put on S(T), paid at T: Black's formula on F(T), DF(T).
    CurveClosedForm black_scholes_vanilla(const ForwardCurve &curve, Real vol, Real strike, Time T, bool is_call);

    /// `payout` paid at T if S(T) > K (call) or S(T) < K (put).
    CurveClosedForm black_scholes_digital(const ForwardCurve &curve, Real vol, Real strike, Time T, bool is_call,
                                          Real payout);

    /// S(T) − K paid at T.
    CurveClosedForm equity_forward(const ForwardCurve &curve, Real strike, Time T);

    /// max(±(G − K), 0) paid at `pay`, G the geometric average of S at the
    /// fixings: ln G is Gaussian, so the price is exact.
    CurveClosedForm geometric_asian(const ForwardCurve &curve, Real vol, Real strike, bool is_call,
                                    const std::vector<Time> &fixings, Time pay);

    /// The arithmetic average's: its first two moments, E[A] = Σ F(t_i)/n and
    /// E[A²] = Σ_ij F(t_i) F(t_j) e^{σ² min(t_i, t_j)}/n², matched to a
    /// lognormal (Levy 1992): an approximation, not the price.
    CurveClosedForm arithmetic_asian_levy(const ForwardCurve &curve, Real vol, Real strike, bool is_call,
                                          const std::vector<Time> &fixings, Time pay);

} // namespace quantModeling

#endif
