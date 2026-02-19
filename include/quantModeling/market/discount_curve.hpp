#ifndef MARKET_DISCOUNT_CURVE_HPP
#define MARKET_DISCOUNT_CURVE_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    class DiscountCurve
    {
    public:
        explicit DiscountCurve(Real flat_rate);
        DiscountCurve(std::vector<Time> times, std::vector<Real> discount_factors);

        Real discount(Time t) const;

    private:
        std::vector<Time> times_;
        std::vector<Real> dfs_;
        Real flat_rate_ = 0.0;
        bool use_flat_rate_ = true;

        void validate_curve() const;
    };

} // namespace quantModeling

#endif
