#include "quantModeling/market/discount_curve.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace quantModeling
{

    DiscountCurve::DiscountCurve(Real flat_rate)
        : flat_rate_(flat_rate), use_flat_rate_(true)
    {
    }

    DiscountCurve::DiscountCurve(std::vector<Time> times, std::vector<Real> discount_factors)
        : times_(std::move(times)), dfs_(std::move(discount_factors)), use_flat_rate_(false)
    {
        validate_curve();
    }

    Real DiscountCurve::discount(Time t) const
    {
        if (t <= 0.0)
            return 1.0;

        if (use_flat_rate_ || times_.empty())
            return std::exp(-flat_rate_ * t);

        if (t <= times_.front())
            return dfs_.front();
        if (t >= times_.back())
            return dfs_.back();

        const auto it = std::upper_bound(times_.begin(), times_.end(), t);
        const size_t idx = static_cast<size_t>(std::distance(times_.begin(), it));
        const Time t1 = times_[idx - 1];
        const Time t2 = times_[idx];
        const Real df1 = dfs_[idx - 1];
        const Real df2 = dfs_[idx];
        const Real w = (t - t1) / (t2 - t1);

        const Real log_df = (1.0 - w) * std::log(df1) + w * std::log(df2);
        return std::exp(log_df);
    }

    void DiscountCurve::validate_curve() const
    {
        if (times_.empty() || dfs_.empty() || times_.size() != dfs_.size())
            throw InvalidInput("DiscountCurve requires matching non-empty times and discount factors");

        Time prev = times_.front();
        if (!(prev > 0.0))
            throw InvalidInput("DiscountCurve times must be > 0");
        if (!(dfs_.front() > 0.0))
            throw InvalidInput("DiscountCurve discount factors must be > 0");

        for (size_t i = 1; i < times_.size(); ++i)
        {
            const Time t = times_[i];
            const Real df = dfs_[i];
            if (!(t > prev))
                throw InvalidInput("DiscountCurve times must be strictly increasing");
            if (!(df > 0.0))
                throw InvalidInput("DiscountCurve discount factors must be > 0");
            prev = t;
        }
    }

} // namespace quantModeling
