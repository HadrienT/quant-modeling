#ifndef QM_ENGINES_XVA_HULL_WHITE_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_HULL_WHITE_FUTURE_VALUE_HPP

#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/instruments/rates/swap.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"

#include <memory>

namespace quantModeling
{

    /**
     * @file hull_white_future_value.hpp
     * @brief Exact future values under the curve-fitted Hull-White model
     *        (blueprint/wp/23-xva.md §13.1, ADR-X1): the state is
     *        x(t) = r(t) - f(0, t) and every zero-coupon bond is a closed
     *        form of it, so swaps and European swaptions reprice exactly at
     *        each date of each path — no regression, no nested simulation.
     *        These exact exposures are the oracle of the regression-based
     *        ones of lot X4.
     *
     * The model is held by reference and must outlive the returned object.
     */

    /**
     * @brief Interest rate swap. At date t its value is
     *
     *   - fixed coupons not yet paid: -K δ N P(t, payment);
     *   - floating coupons not yet fixed: N (β P(t, start) - P(t, end)),
     *     the same decomposition as swap_as_bonds;
     *   - the floating coupon in progress, fixed at its start date s < t on
     *     this path: N τ (L(s) + spread) P(t, end), with
     *     1 + τ L(s) = β / P(s, end | x(s)).
     *
     * @throws InvalidInput if a coupon starts in the past (a seasoned swap
     *         would need its historical fixing) or if a floating coupon does
     *         not pay on its period end.
     */
    std::unique_ptr<FutureValue> make_future_value(const InterestRateSwap &swap,
                                                   const HullWhiteCurveModel &model);

    /**
     * @brief European swaption, physically settled, **bought** (a sold one is
     *        a negative quantity in the engine).
     *
     * Before the expiry: the closed-form price given x(t)
     * (hull_white_european_swaption). At the expiry: exercised on the paths
     * where the swap is worth more than zero. After: the swap itself on
     * those paths — with its negative values, the exposure of a swap — and
     * nothing on the others.
     */
    std::unique_ptr<FutureValue> make_future_value(const Swaption &swaption,
                                                   const HullWhiteCurveModel &model);

    /**
     * @brief Dispatch on the instrument by the visitor, as the pricing
     *        engines do.
     * @throws UnsupportedInstrument for anything without a closed-form future
     *         value under Hull-White (Bermudans and scripts come with the
     *         regression of lot X4).
     */
    std::unique_ptr<FutureValue> make_hull_white_future_value(const Instrument &instrument,
                                                              const HullWhiteCurveModel &model);

} // namespace quantModeling

#endif // QM_ENGINES_XVA_HULL_WHITE_FUTURE_VALUE_HPP
