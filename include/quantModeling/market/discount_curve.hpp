#ifndef MARKET_DISCOUNT_CURVE_HPP
#define MARKET_DISCOUNT_CURVE_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    /// What discount() does outside the pillars.
    enum class CurveExtrapolation
    {
        /// Discount factor held flat: DF(t) = DF(t_1) before the first
        /// pillar, DF(t_n) past the last (a zero forward rate there). The
        /// historical behaviour, kept for the curves built with it.
        FlatDiscountFactor,
        /// Forward rate held flat: log-linear from (0, 1) to the first
        /// pillar, the last segment's forward past the last one. What a
        /// projection curve needs, whose forwards before its first pillar
        /// must not vanish (blueprint/wp/21-rates.md §1).
        FlatForward
    };

    class DiscountCurve
    {
      public:
        explicit DiscountCurve(Real flat_rate);
        DiscountCurve(std::vector<Time> times, std::vector<Real> discount_factors,
                      CurveExtrapolation extrapolation = CurveExtrapolation::FlatDiscountFactor);

        Real discount(Time t) const;

        /// The pillars between which discount() interpolates log-linearly
        /// (empty for a flat-rate curve) — where the forward rate may jump.
        const std::vector<Time> &pillar_times() const { return times_; }
        const std::vector<Real> &pillar_discount_factors() const { return dfs_; }
        CurveExtrapolation extrapolation() const { return extrapolation_; }

      private:
        std::vector<Time> times_;
        std::vector<Real> dfs_;
        Real flat_rate_ = 0.0;
        bool use_flat_rate_ = true;
        CurveExtrapolation extrapolation_ = CurveExtrapolation::FlatDiscountFactor;

        void validate_curve() const;
    };

} // namespace quantModeling

#endif
