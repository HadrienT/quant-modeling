#ifndef ENGINE_ANALYTIC_HULL_WHITE_SWAPTION_HPP
#define ENGINE_ANALYTIC_HULL_WHITE_SWAPTION_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/rates/swap.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <utility>
#include <vector>

namespace quantModeling
{

    /// A cash amount at `time`, valued at an earlier date t as
    /// amount · P(t, time).
    struct BondCashflow
    {
        Time time;
        Real amount;
    };

    /**
     * @brief The coupons of `swap` starting on or after `exercise`, as a
     *        linear combination of zero-coupon bonds seen from `exercise`:
     *        value at `exercise` = Σ amount_k P(exercise, time_k | x).
     *
     * Fixed coupon: ∓ K δ N at its payment. Floating coupon on [s, e]:
     * ± N (β P(·, s) − P(·, e)) plus the spread ± N m τ P(·, e), with the
     * model's deterministic basis β (HullWhiteCurveModel). Signs are the
     * swap's side (+ floating for a payer). Floating coupons must pay on
     * their period end (no payment-delay convexity in this model).
     */
    std::vector<BondCashflow> swap_as_bonds(const InterestRateSwap &swap, Time exercise,
                                            const HullWhiteCurveModel &model);

    /**
     * @brief European swaption under Hull-White, in closed form.
     *
     * Under the expiry-T forward measure x(T) is centred Gaussian with
     * variance y(T), and the exercise value is Σ c_k A_k e^{−G_k x − ½G_k² y}.
     * Each term integrates in closed form over any interval of x; the
     * exercise region (where the swap is worth > 0) is found by locating
     * every sign change on a grid of ±12 sd (480 cells) and polishing it by
     * bisection. Jamshidian's (1989) decomposition is the single-root case
     * of this; the grid also covers the multi-curve cash flows, whose
     * signs need not alternate once.
     */
    Real hull_white_european_swaption(const Swaption &swaption, const HullWhiteCurveModel &model);

    /**
     * @brief Where a European swaption is exercised, as a set of intervals of
     *        x(expiry). It depends on the contract and on the model, not on
     *        the valuation date: computed once, it prices the swaption at any
     *        earlier date and state (the exposure engine of
     *        blueprint/wp/23-xva.md §13.3 does so on every path).
     */
    struct HullWhiteExerciseRegion
    {
        Time expiry = 0.0;
        /// Exercise value = Σ amount_k P(expiry, time_k | x).
        std::vector<BondCashflow> bonds;
        /// P(0, time_k) / P(0, expiry) and G(expiry, time_k), per bond.
        std::vector<Real> forward_bond;
        std::vector<Real> G;
        /// Where the exercise value is > 0; the outer bounds may be ±infinity.
        std::vector<std::pair<Real, Real>> intervals;
    };

    HullWhiteExerciseRegion hull_white_exercise_region(const Swaption &swaption,
                                                       const HullWhiteCurveModel &model);

    /**
     * @brief Value at `t <= expiry` of the swaption, given the state x(t).
     *
     * Under the expiry-forward measure x(expiry) given x(t) is Gaussian
     * (HullWhiteCurveModel::transition), so each bond integrates in closed
     * form over the exercise region, as at t = 0.
     */
    Real hull_white_european_swaption(const HullWhiteExerciseRegion &region,
                                      const HullWhiteCurveModel &model, Time t, Real x);

    /**
     * @brief The same value, bond by bond: `terms[k]` is what the cash flow
     *        `region.bonds[k]` contributes,
     *
     *   amount_k P(t, time_k) Q^{time_k}(exercise | x(t)),
     *
     * and they add up to the value. Each is also the sensitivity of the
     * value to the relative move of P(t, time_k): the exercise boundary is
     * where the swap is worth nothing, so moving it changes nothing at first
     * order (Jamshidian's decomposition says the same: a swaption is a
     * portfolio of bond options whose strikes cancel).
     */
    void hull_white_european_swaption_terms(const HullWhiteExerciseRegion &region,
                                            const HullWhiteCurveModel &model, Time t, Real x,
                                            std::vector<Real> &terms);

    /// Settings of the Bermudan lattice.
    struct HullWhiteLatticeSettings
    {
        int grid_points = 401;    ///< odd: the grid includes x = 0
        Real grid_std_devs = 8.0; ///< half-width, in sd of x at the last exercise
    };

    /**
     * @brief Bermudan swaption under Hull-White by backward induction on a
     *        grid in x (blueprint/wp/21-rates.md §3).
     *
     * Works under the terminal measure (numeraire P(·, T_N), T_N the swap's
     * maturity), where x has exact Gaussian transitions between exercise
     * dates (HullWhiteCurveModel::transition). The deflated value is held
     * on the grid, interpolated linearly between nodes, and each
     * conditional expectation of that piecewise-linear function against the
     * Gaussian transition is computed in closed form — no quadrature error,
     * only the O(h²) of the interpolation. With one exercise date it
     * reproduces hull_white_european_swaption (the test).
     */
    Real hull_white_bermudan_swaption(const BermudanSwaption &bermudan, const HullWhiteCurveModel &model,
                                      const HullWhiteLatticeSettings &settings = {});

    /**
     * @brief Prices swaps, European and Bermudan swaptions under a
     *        HullWhiteCurveModel (the context's model). NPV only.
     */
    class HullWhiteSwaptionEngine final : public EngineBase
    {
      public:
        using EngineBase::EngineBase;

        void visit(const InterestRateSwap &swap) override;
        void visit(const Swaption &swaption) override;
        void visit(const BermudanSwaption &bermudan) override;

        void visit(const VanillaOption &) override { unsupported("vanilla option"); }
        void visit(const AsianOption &) override { unsupported("Asian option"); }
        void visit(const BarrierOption &) override { unsupported("barrier option"); }
        void visit(const DigitalOption &) override { unsupported("digital option"); }
        void visit(const EquityFuture &) override { unsupported("equity future"); }
        void visit(const ZeroCouponBond &) override { unsupported("zero-coupon bond"); }
        void visit(const FixedRateBond &) override { unsupported("fixed-rate bond"); }
    };

} // namespace quantModeling

#endif
