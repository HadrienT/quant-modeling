#include "quantModeling/market/credit_curve.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{

    CreditCurve::CreditCurve(Real flat_hazard)
        : CreditCurve(std::vector<Time>{1.0}, std::vector<Real>{flat_hazard})
    {
    }

    CreditCurve::CreditCurve(std::vector<Time> times, std::vector<Real> hazards)
        : times_(std::move(times)), hazards_(std::move(hazards))
    {
        if (times_.empty() || times_.size() != hazards_.size())
            throw InvalidInput("CreditCurve requires matching non-empty times and hazard rates");
        Real integral = 0.0;
        Time previous = 0.0;
        cumulative_.reserve(times_.size());
        for (std::size_t i = 0; i < times_.size(); ++i)
        {
            if (!(times_[i] > previous))
                throw InvalidInput("CreditCurve times must be > 0 and strictly increasing");
            if (!(hazards_[i] >= 0.0) || !std::isfinite(hazards_[i]))
                throw InvalidInput("CreditCurve hazard rates must be finite and >= 0 (got " +
                                   std::to_string(hazards_[i]) + " on the segment ending at t=" +
                                   std::to_string(times_[i]) + ")");
            integral += hazards_[i] * (times_[i] - previous);
            cumulative_.push_back(integral);
            previous = times_[i];
        }
    }

    Real CreditCurve::hazard(Time t) const
    {
        // Segment i is (times[i-1], times[i]]; lower_bound finds it.
        const auto it = std::lower_bound(times_.begin(), times_.end(), t);
        return it == times_.end() ? hazards_.back()
                                  : hazards_[static_cast<std::size_t>(it - times_.begin())];
    }

    Real CreditCurve::survival(Time t) const
    {
        if (t <= 0.0)
            return 1.0;
        const auto it = std::lower_bound(times_.begin(), times_.end(), t);
        const std::size_t i = static_cast<std::size_t>(it - times_.begin());
        const Real before = i == 0 ? 0.0 : cumulative_[i - 1];
        const Time start = i == 0 ? 0.0 : times_[i - 1];
        const Real lambda = i == times_.size() ? hazards_.back() : hazards_[i];
        return std::exp(-(before + lambda * (t - start)));
    }

    Real CreditCurve::conditional_default_probability(Time t1, Time t2) const
    {
        if (t2 < t1)
            throw InvalidInput("conditional_default_probability: t2 must be >= t1");
        return 1.0 - survival(t2) / survival(t1);
    }

} // namespace quantModeling
