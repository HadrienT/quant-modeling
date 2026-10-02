#ifndef QM_ENGINES_XVA_REGRESSION_FUTURE_VALUE_HPP
#define QM_ENGINES_XVA_REGRESSION_FUTURE_VALUE_HPP

#include "quantModeling/engines/xva/future_value.hpp"
#include "quantModeling/scripting/exercise.hpp"

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

    struct AmcSettings
    {
        /// Total degree of the polynomial fitted in each bucket; 0 picks 2.
        int degree = 0;
        /// Buckets of the first regressor (the model state), with equal
        /// numbers of pilot paths, each with its own polynomial; 0 picks 4.
        /// One bucket is a single global polynomial.
        int buckets = 0;
    };

    /**
     * @brief A polynomial least-squares fit, evaluated without allocating:
     *        value() calls it for every date of every path.
     *
     * The fit is scripting::fit_exercise_regression — the regression the
     * scripts' exercise() already uses (standardised regressors, pivoted
     * QR). With too few points for its degree it falls back to a lower one,
     * down to the mean; with none it predicts 0.
     */
    class PolynomialRegression
    {
      public:
        PolynomialRegression() = default;

        /// `rows` holds `count` rows of `n` regressors each, one after the other.
        static PolynomialRegression fit(const std::vector<Real> &rows, std::size_t n,
                                        const std::vector<Real> &targets, int degree);

        Real operator()(const Real *regressors) const;
        bool empty() const { return fit_.beta.empty(); }
        int degree() const { return fit_.degree; }

      private:
        scripting::ExerciseRegression fit_;
    };

    /**
     * @brief A regression by buckets: the pilot paths are sorted by their
     *        first regressor and cut into buckets of equal size, each fitted
     *        by its own low-degree polynomial.
     *
     * One global polynomial cannot follow the kink of an option near its
     * expiry: it oscillates around it and runs away in the tails, which is
     * where a 99 % PFE is read. A local fit follows the kink and stays
     * bounded by the data of its own bucket. The estimate jumps slightly
     * from one bucket to the next, which an exposure statistic does not see.
     */
    class BucketedRegression
    {
      public:
        BucketedRegression() = default;

        /// `rows` holds `targets.size()` rows of `n` regressors each.
        static BucketedRegression fit(const std::vector<Real> &rows, std::size_t n,
                                      const std::vector<Real> &targets, const AmcSettings &settings);

        Real operator()(const Real *regressors) const;
        bool empty() const { return buckets_.empty(); }
        std::size_t buckets() const { return buckets_.size(); }

      private:
        /// Upper edges of every bucket but the last, on the first regressor.
        std::vector<Real> edges_;
        std::vector<PolynomialRegression> buckets_;
    };

    /// What AmcSettings' zeros stand for.
    /// @throws InvalidInput on a negative or oversized setting.
    AmcSettings resolved(const AmcSettings &settings);

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
