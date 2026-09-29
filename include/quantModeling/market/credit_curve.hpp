#ifndef QM_MARKET_CREDIT_CURVE_HPP
#define QM_MARKET_CREDIT_CURVE_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief Survival curve of a reference entity, as a piecewise-constant
     *        hazard rate (blueprint: etc/roadmap.md, chantier 4b).
     *
     * `hazards[i]` is the default intensity on (times[i-1], times[i]], with
     * times[-1] = 0; the last one extends flat past the last pillar. Then
     *   S(t) = exp(-∫_0^t λ(u) du),    P(default before t) = 1 - S(t).
     *
     * Piecewise-constant intensity is the market standard for single-name
     * credit (the ISDA CDS Standard Model, O'Kane 2008, ch. 3 and 7): it is
     * what a CDS bootstrap produces, one hazard per quoted maturity, and it
     * makes the CDS legs integrable in closed form between knots.
     *
     * Convention-agnostic like DiscountCurve: times are year fractions on
     * whatever basis the caller chose.
     */
    class CreditCurve
    {
      public:
        /// A flat hazard rate λ: S(t) = exp(-λ t).
        explicit CreditCurve(Real flat_hazard);

        /// @throws InvalidInput on empty or mismatched vectors, non-increasing
        ///         or non-positive times, or a negative hazard rate (a
        ///         survival probability that rises is an arbitrage).
        CreditCurve(std::vector<Time> times, std::vector<Real> hazards);

        Real survival(Time t) const;
        Real default_probability(Time t) const { return 1.0 - survival(t); }
        /// Instantaneous hazard λ(t) (right-continuous at the pillars).
        Real hazard(Time t) const;
        /// Default probability in (t1, t2] conditional on survival to t1.
        Real conditional_default_probability(Time t1, Time t2) const;

        const std::vector<Time> &times() const { return times_; }
        const std::vector<Real> &hazards() const { return hazards_; }

      private:
        std::vector<Time> times_;
        std::vector<Real> hazards_;
        /// ∫_0^{times_[i]} λ, cached so survival() is one lookup.
        std::vector<Real> cumulative_;
    };

} // namespace quantModeling

#endif // QM_MARKET_CREDIT_CURVE_HPP
