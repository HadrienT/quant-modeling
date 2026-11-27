#include "quantModeling/market/forward_curve.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace quantModeling
{

    namespace
    {
        void check_spot(Real spot)
        {
            if (!(spot > 0.0) || !std::isfinite(spot))
                throw InvalidInput("ForwardCurve spot must be finite and > 0");
        }
    } // namespace

    ForwardCurve::ForwardCurve(Real spot, Real rate, Real dividend)
        : spot_(spot), discount_(rate), flat_(true), flat_rate_(rate), flat_dividend_(dividend)
    {
        check_spot(spot);
        if (!std::isfinite(rate) || !std::isfinite(dividend))
            throw InvalidInput("ForwardCurve flat rate and dividend must be finite");
    }

    ForwardCurve ForwardCurve::flat(Real spot, Real rate, Real dividend)
    {
        return ForwardCurve(spot, rate, dividend);
    }

    ForwardCurve::ForwardCurve(Real spot, DiscountCurve discount, std::vector<Time> times,
                               std::vector<Real> forwards)
        : spot_(spot), discount_(std::move(discount)), times_(std::move(times))
    {
        check_spot(spot);
        if (times_.empty() || times_.size() != forwards.size())
            throw InvalidInput("ForwardCurve requires matching non-empty times and forwards");
        log_fwd_.reserve(forwards.size());
        Time prev = 0.0;
        for (std::size_t i = 0; i < times_.size(); ++i)
        {
            if (!(times_[i] > prev) || !std::isfinite(times_[i]))
                throw InvalidInput("ForwardCurve times must be > 0 and strictly increasing");
            if (!(forwards[i] > 0.0) || !std::isfinite(forwards[i]))
                throw InvalidInput("ForwardCurve forwards must be finite and > 0");
            log_fwd_.push_back(std::log(forwards[i] / spot_));
            prev = times_[i];
        }
    }

    ForwardCurve ForwardCurve::from_pillars(Real spot, std::vector<Time> times, std::vector<Real> forwards,
                                            std::vector<Real> discount_factors)
    {
        DiscountCurve discount(times, std::move(discount_factors), CurveExtrapolation::FlatForward);
        return ForwardCurve(spot, std::move(discount), std::move(times), std::move(forwards));
    }

    ForwardCurve ForwardCurve::shifted(Real rate_shift, Real dividend_shift) const
    {
        if (flat_)
            return flat(spot_, flat_rate_ + rate_shift, flat_dividend_ + dividend_shift);
        const std::vector<Time> &dt = discount_.pillar_times();
        DiscountCurve discount = [&]
        {
            if (dt.empty()) // a flat-rate discount curve
                return DiscountCurve(-std::log(discount_.discount(1.0)) + rate_shift);
            std::vector<Real> dfs;
            dfs.reserve(dt.size());
            for (std::size_t i = 0; i < dt.size(); ++i)
                dfs.push_back(discount_.pillar_discount_factors()[i] * std::exp(-rate_shift * dt[i]));
            return DiscountCurve(dt, std::move(dfs), discount_.extrapolation());
        }();
        std::vector<Real> forwards;
        forwards.reserve(times_.size());
        for (std::size_t i = 0; i < times_.size(); ++i)
            forwards.push_back(spot_ * std::exp(log_fwd_[i] + (rate_shift - dividend_shift) * times_[i]));
        return ForwardCurve(spot_, std::move(discount), times_, std::move(forwards));
    }

    Real ForwardCurve::log_forward(Time t) const
    {
        if (flat_)
            return (flat_rate_ - flat_dividend_) * t;
        if (t <= 0.0)
            return 0.0;
        const std::size_t n = times_.size();
        if (t <= times_.front())
            return log_fwd_.front() * t / times_.front();
        if (t >= times_.back())
        {
            const Real slope = n == 1 ? log_fwd_[0] / times_[0]
                                      : (log_fwd_[n - 1] - log_fwd_[n - 2]) / (times_[n - 1] - times_[n - 2]);
            return log_fwd_.back() + slope * (t - times_.back());
        }
        const auto it = std::upper_bound(times_.begin(), times_.end(), t);
        const std::size_t idx = static_cast<std::size_t>(std::distance(times_.begin(), it));
        const Real w = (t - times_[idx - 1]) / (times_[idx] - times_[idx - 1]);
        return (1.0 - w) * log_fwd_[idx - 1] + w * log_fwd_[idx];
    }

    Real ForwardCurve::forward(Time t) const
    {
        if (flat_)
            return spot_ * std::exp((flat_rate_ - flat_dividend_) * t);
        return spot_ * std::exp(log_forward(t));
    }

    Real ForwardCurve::carry(Time t1, Time t2) const
    {
        if (flat_)
            return (flat_rate_ - flat_dividend_) * (t2 - t1);
        return log_forward(t2) - log_forward(t1);
    }

    Real ForwardCurve::rate(Time t1, Time t2) const
    {
        if (flat_)
            return flat_rate_ * (t2 - t1);
        return std::log(discount_.discount(t1)) - std::log(discount_.discount(t2));
    }

    Real ForwardCurve::dividend(Time t1, Time t2) const
    {
        if (flat_)
            return flat_dividend_ * (t2 - t1);
        return rate(t1, t2) - carry(t1, t2);
    }

    void ForwardCurve::log_forward_weights(Time t, std::vector<std::pair<std::size_t, Real>> &weights) const
    {
        weights.clear();
        if (flat_ || t <= 0.0)
            return;
        const std::size_t n = times_.size();
        if (t <= times_.front())
        {
            weights.emplace_back(0, t / times_.front());
            return;
        }
        if (t >= times_.back())
        {
            if (n == 1)
            {
                weights.emplace_back(0, t / times_[0]);
                return;
            }
            // y_n + (t − t_n) (y_n − y_{n−1}) / (t_n − t_{n−1})
            const Real k = (t - times_[n - 1]) / (times_[n - 1] - times_[n - 2]);
            weights.emplace_back(n - 1, 1.0 + k);
            weights.emplace_back(n - 2, -k);
            return;
        }
        const auto it = std::upper_bound(times_.begin(), times_.end(), t);
        const std::size_t idx = static_cast<std::size_t>(std::distance(times_.begin(), it));
        const Real w = (t - times_[idx - 1]) / (times_[idx] - times_[idx - 1]);
        weights.emplace_back(idx - 1, 1.0 - w);
        weights.emplace_back(idx, w);
    }

} // namespace quantModeling
