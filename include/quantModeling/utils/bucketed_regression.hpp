#ifndef QM_UTILS_BUCKETED_REGRESSION_HPP
#define QM_UTILS_BUCKETED_REGRESSION_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/scripting/exercise.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    /**
     * @file bucketed_regression.hpp
     * @brief Least-squares regression of a noisy target on a few regressors,
     *        evaluated without allocating: what American Monte-Carlo
     *        (engines/xva/regression_future_value.hpp) and the dynamic
     *        initial margin (risk/initial_margin.hpp) both estimate a
     *        conditional expectation with.
     */

    /// Most regressors one fit can have.
    inline constexpr std::size_t kMaxRegressors = 8;

    struct RegressionSettings
    {
        /// Total degree of the polynomial fitted in each bucket; 0 picks 2.
        int degree = 0;
        /// Buckets of the first regressor, with equal numbers of points,
        /// each with its own polynomial; 0 picks 4. One bucket is a single
        /// global polynomial.
        int buckets = 0;
        /**
         * @brief λ of the differential regression (blueprint/wp/25-ml.md
         *        §3.3, lot M2): the weight of the targets' derivatives with
         *        respect to the first regressor, on the scale of the targets
         *        (scripting::fit_differential_regression). 0, the default, is
         *        the plain regression, to the bit; it applies only where the
         *        caller has the derivatives.
         */
        Real differential = 0.0;
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
        /// The differential fit: `derivatives` holds d target / d first
        /// regressor, one per row; λ = 0 is fit() above, to the bit.
        static PolynomialRegression fit(const std::vector<Real> &rows, std::size_t n,
                                        const std::vector<Real> &targets,
                                        const std::vector<Real> &derivatives, int degree, Real lambda);

        Real operator()(const Real *regressors) const;
        bool empty() const { return fit_.beta.empty(); }
        int degree() const { return fit_.degree; }
        /// The fitted coefficients, for an evaluation elsewhere (the
        /// device's, risk/initial_margin_path.hpp).
        const scripting::ExerciseRegression &coefficients() const { return fit_; }

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
                                      const std::vector<Real> &targets, const RegressionSettings &settings);
        /// The same buckets, each a differential fit with λ =
        /// settings.differential (`derivatives`: d target / d first
        /// regressor, one per row).
        static BucketedRegression fit(const std::vector<Real> &rows, std::size_t n,
                                      const std::vector<Real> &targets, const std::vector<Real> &derivatives,
                                      const RegressionSettings &settings);

        Real operator()(const Real *regressors) const;
        bool empty() const { return buckets_.empty(); }
        std::size_t buckets() const { return buckets_.size(); }
        /// Upper edges of every bucket but the last, on the first regressor:
        /// a value goes to the first bucket whose edge is above it.
        const std::vector<Real> &edges() const { return edges_; }
        const PolynomialRegression &bucket(std::size_t b) const { return buckets_[b]; }

      private:
        /// Upper edges of every bucket but the last, on the first regressor.
        std::vector<Real> edges_;
        std::vector<PolynomialRegression> buckets_;
    };

    /// What RegressionSettings' zeros stand for.
    /// @throws InvalidInput on a negative or oversized setting.
    RegressionSettings resolved(const RegressionSettings &settings);

} // namespace quantModeling

#endif // QM_UTILS_BUCKETED_REGRESSION_HPP
