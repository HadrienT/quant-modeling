#ifndef QM_ENGINES_XVA_REGRESSION_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_REGRESSION_FUTURE_VALUE_HPP

#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/utils/bucketed_regression.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace quantModeling
{

    /**
     * @file regression_future_value.hpp
     * @brief American Monte-Carlo (blueprint/wp/23-xva.md §13.4, lot X4):
     *        the value of a trade at a future date, on a path, estimated by
     *        regression instead of a closed form.
     *
     *   V(t_i) = E[ Σ_{j>i} flow(t_j) N(t_i) / N(t_j) | F(t_i) ]
     *          ≈ β_i · φ(regressors at t_i),
     *
     * with N the numeraire and φ a polynomial basis: the regression of
     * Longstaff & Schwartz (2001) and Carriere (1996), carried from the
     * exercise dates of an option to **every date of the exposure grid**.
     * The coefficients come from a pilot simulation independent of the main
     * one, so that the paths the exposure is measured on were not used to
     * fit it (no foresight bias).
     *
     * Closed forms stay the rule where they exist (ADR-X1): they are exact,
     * and they are the oracle these regressions are tested against.
     */

    /// The regression of a trade's future value: degree and buckets of the
    /// state (utils/bucketed_regression.hpp).
    using AmcSettings = RegressionSettings;

    /**
     * @brief The regression twin of a trade: same cash flows, same regimes
     *        and regressors, but a value estimated by regressing its future
     *        cash flows instead of `trade`'s own value().
     *
     * For a trade that has a closed form this is the test of the method
     * (§15: "exposure by regression = closed form"); for one described only
     * by what it pays, it is its valuation.
     */
    std::unique_ptr<FutureValue> make_regression_future_value(std::unique_ptr<FutureValue> trade,
                                                              const AmcSettings &settings = {});

} // namespace quantModeling

#endif // QM_ENGINES_XVA_REGRESSION_FUTURE_VALUE_HPP
