#include "quantModeling/scripting/domain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling::scripting
{
    namespace
    {
        constexpr double kInf = std::numeric_limits<double>::infinity();

        /// 0 * inf is taken as 0 (a limit along the axis), otherwise ordinary.
        double mul(double x, double y)
        {
            if (x == 0.0 || y == 0.0)
                return 0.0;
            return x * y;
        }
    } // namespace

    bool Domain::Interval::is_singleton() const
    {
        return lo == hi && std::isfinite(lo);
    }

    bool Domain::Interval::contains(double v) const { return lo <= v && v <= hi; }

    // ── construction ─────────────────────────────────────────────────────────

    Domain Domain::singleton(double v)
    {
        Domain d;
        d.parts_.push_back({v, v});
        return d;
    }

    Domain Domain::closed(double lo, double hi)
    {
        Domain d;
        if (lo <= hi)
            d.parts_.push_back({lo, hi});
        return d;
    }

    Domain Domain::greater_than(double lo)
    {
        // strictly-open lower bound: nudge it up by one ULP so `(0, +inf)`
        // reads as all-positive (the endpoint is not attained).
        return closed(std::nextafter(lo, kInf), kInf);
    }

    Domain Domain::at_least(double lo) { return closed(lo, kInf); }

    Domain Domain::real_line() { return closed(-kInf, kInf); }

    // ── normalisation ────────────────────────────────────────────────────────

    void Domain::normalise()
    {
        if (parts_.size() < 2)
            return;
        std::sort(parts_.begin(), parts_.end(),
                  [](const Interval &a, const Interval &b)
                  { return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi); });
        std::vector<Interval> merged;
        merged.push_back(parts_.front());
        for (std::size_t i = 1; i < parts_.size(); ++i)
        {
            Interval &last = merged.back();
            const Interval &cur = parts_[i];
            if (cur.lo <= last.hi) // overlap or touch -> fuse
                last.hi = std::max(last.hi, cur.hi);
            else
                merged.push_back(cur);
        }
        parts_ = std::move(merged);
    }

    void Domain::add_interval(Interval iv)
    {
        if (iv.lo <= iv.hi)
        {
            parts_.push_back(iv);
            normalise();
        }
    }

    // ── predicates ───────────────────────────────────────────────────────────

    bool Domain::is_discrete() const
    {
        if (parts_.empty())
            return false;
        for (const Interval &iv : parts_)
            if (!iv.is_singleton())
                return false;
        return true;
    }

    bool Domain::contains(double v) const
    {
        for (const Interval &iv : parts_)
            if (iv.contains(v))
                return true;
        return false;
    }

    bool Domain::all_positive() const
    {
        if (parts_.empty())
            return false;
        for (const Interval &iv : parts_)
            if (iv.lo <= 0.0)
                return false;
        return true;
    }

    bool Domain::all_negative() const
    {
        if (parts_.empty())
            return false;
        for (const Interval &iv : parts_)
            if (iv.hi >= 0.0)
                return false;
        return true;
    }

    bool Domain::all_non_negative() const
    {
        if (parts_.empty())
            return false;
        for (const Interval &iv : parts_)
            if (iv.lo < 0.0)
                return false;
        return true;
    }

    bool Domain::all_non_positive() const
    {
        if (parts_.empty())
            return false;
        for (const Interval &iv : parts_)
            if (iv.hi > 0.0)
                return false;
        return true;
    }

    double Domain::distance_to_zero() const
    {
        double best = kInf;
        for (const Interval &iv : parts_)
        {
            if (iv.lo <= 0.0 && 0.0 <= iv.hi)
                return 0.0;
            best = std::min(best, std::min(std::fabs(iv.lo), std::fabs(iv.hi)));
        }
        return best;
    }

    // ── arithmetic ───────────────────────────────────────────────────────────

    Domain Domain::operator-() const
    {
        Domain d;
        for (const Interval &iv : parts_)
            d.parts_.push_back({-iv.hi, -iv.lo});
        d.normalise();
        return d;
    }

    Domain Domain::operator+(const Domain &other) const
    {
        Domain d;
        for (const Interval &a : parts_)
            for (const Interval &b : other.parts_)
                d.add_interval({a.lo + b.lo, a.hi + b.hi});
        return d;
    }

    Domain Domain::operator-(const Domain &other) const
    {
        return *this + (-other);
    }

    Domain Domain::operator*(const Domain &other) const
    {
        Domain d;
        for (const Interval &a : parts_)
            for (const Interval &b : other.parts_)
            {
                const double c1 = mul(a.lo, b.lo);
                const double c2 = mul(a.lo, b.hi);
                const double c3 = mul(a.hi, b.lo);
                const double c4 = mul(a.hi, b.hi);
                d.add_interval({std::min({c1, c2, c3, c4}),
                                std::max({c1, c2, c3, c4})});
            }
        return d;
    }

    Domain Domain::operator/(const Domain &other) const
    {
        // Only the common, well-defined case: divide by a non-zero constant.
        if (other.parts_.size() == 1 && other.parts_.front().is_singleton() &&
            other.parts_.front().lo != 0.0)
            return *this * singleton(1.0 / other.parts_.front().lo);
        return real_line();
    }

    // ── free functions ───────────────────────────────────────────────────────

    Domain domain_union(const Domain &a, const Domain &b)
    {
        Domain d = a;
        for (const Domain::Interval &iv : b.parts_)
            d.parts_.push_back(iv);
        d.normalise();
        return d;
    }

    Domain domain_min(const Domain &a, const Domain &b)
    {
        Domain d;
        for (const Domain::Interval &x : a.parts_)
            for (const Domain::Interval &y : b.parts_)
                d.add_interval(
                    {std::min(x.lo, y.lo), std::min(x.hi, y.hi)});
        return d;
    }

    Domain domain_max(const Domain &a, const Domain &b)
    {
        Domain d;
        for (const Domain::Interval &x : a.parts_)
            for (const Domain::Interval &y : b.parts_)
                d.add_interval(
                    {std::max(x.lo, y.lo), std::max(x.hi, y.hi)});
        return d;
    }

    Domain domain_abs(const Domain &a)
    {
        Domain d;
        for (const Domain::Interval &iv : a.parts_)
        {
            if (iv.lo >= 0.0)
                d.add_interval(iv);
            else if (iv.hi <= 0.0)
                d.add_interval({-iv.hi, -iv.lo});
            else
                d.add_interval({0.0, std::max(-iv.lo, iv.hi)});
        }
        return d;
    }

    Domain domain_exp(const Domain &a)
    {
        Domain d;
        for (const Domain::Interval &iv : a.parts_)
            d.add_interval({std::exp(iv.lo), std::exp(iv.hi)});
        return d;
    }

    Domain domain_log(const Domain &a)
    {
        if (!a.all_positive())
            return Domain::real_line();
        Domain d;
        for (const Domain::Interval &iv : a.parts_)
            d.add_interval({std::log(iv.lo), std::log(iv.hi)});
        return d;
    }

    Domain domain_sqrt(const Domain &a)
    {
        if (!a.all_non_negative())
            return Domain::real_line();
        Domain d;
        for (const Domain::Interval &iv : a.parts_)
            d.add_interval({std::sqrt(iv.lo), std::sqrt(iv.hi)});
        return d;
    }

} // namespace quantModeling::scripting
