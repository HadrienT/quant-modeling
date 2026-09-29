#ifndef QM_SCRIPTING_EXERCISE_HPP
#define QM_SCRIPTING_EXERCISE_HPP

#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief The regression of one exercise date (Longstaff & Schwartz 2001):
     *        E[exercise value − continuation | regressors] ≈ β · φ(z), z the
     *        regressors standardised on the pilot paths, φ every monomial of
     *        total degree ≤ `degree` in z.
     */
    struct ExerciseRegression
    {
        std::vector<double> mean;  ///< per regressor, on the pilot paths
        std::vector<double> scale; ///< per regressor (1 when it did not vary)
        int degree = 2;
        std::vector<std::vector<int>> terms; ///< the monomials, monomials(n, degree)
        std::vector<double> beta;            ///< one per monomial

        /// β · φ(z): the estimated gain of exercising now over continuing,
        /// in deflated currency, from the holder's side.
        double gain(const std::vector<double> &regressors) const;
    };

    /// Monomials of total degree ≤ d in n variables, as exponent vectors, in
    /// a fixed order (constant first).
    std::vector<std::vector<int>> monomials(std::size_t n, int degree);

    /// φ(z) for those monomials.
    std::vector<double> basis(const std::vector<double> &z, const std::vector<std::vector<int>> &terms);

    /**
     * @brief The exercise rule a script is priced under: one regression per
     *        event (index in the product's timeline) at which the script
     *        reached exercise() / call() on the pilot paths.
     *
     * The holder exercises when the estimated gain is positive; the issuer
     * calls when it is negative (calling takes away from the holder what the
     * holder would lose by it). An event without a regression never
     * exercises (no pilot path reached it).
     */
    struct ExercisePolicy
    {
        bool issuer = false;
        std::map<std::size_t, ExerciseRegression> by_event;

        bool decide(std::size_t event, const std::vector<double> &regressors) const;
        bool empty() const { return by_event.empty(); }
    };

    /**
     * @brief Least squares fit of `targets` on the basis of `rows`
     *        (Longstaff & Schwartz's regression), standardising each column.
     *        Solved by column-pivoting QR, so a basis that is rank deficient
     *        on the data (a regressor constant at this date) still fits.
     */
    ExerciseRegression fit_exercise_regression(const std::vector<std::vector<double>> &rows,
                                               const std::vector<double> &targets, int degree);

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_EXERCISE_HPP
