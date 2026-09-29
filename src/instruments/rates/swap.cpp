#include "quantModeling/instruments/rates/swap.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{
    namespace
    {
        std::vector<CouponPeriod> regular_leg(Time start, Time tenor, int frequency, const char *leg)
        {
            if (frequency <= 0)
                throw InvalidInput(std::string("make_swap: ") + leg + " frequency must be > 0");
            const Real n_real = tenor * frequency;
            const long n = std::lround(n_real);
            if (n <= 0 || std::abs(n_real - static_cast<Real>(n)) > 1e-9)
                throw InvalidInput(std::string("make_swap: the ") + leg +
                                   " leg needs tenor x frequency to be a whole number of periods");
            std::vector<CouponPeriod> periods;
            periods.reserve(static_cast<std::size_t>(n));
            const Real dt = 1.0 / frequency;
            for (long i = 0; i < n; ++i)
            {
                const Time s = start + static_cast<Real>(i) * dt;
                const Time e = (i + 1 == n) ? start + tenor : start + static_cast<Real>(i + 1) * dt;
                periods.push_back({s, e, e, e - s});
            }
            return periods;
        }
    } // namespace

    Time InterestRateSwap::start() const
    {
        Time s = fixed_leg.empty() ? floating_leg.front().start : fixed_leg.front().start;
        if (!floating_leg.empty())
            s = std::min(s, floating_leg.front().start);
        return s;
    }

    Time InterestRateSwap::maturity() const
    {
        Time m = 0.0;
        for (const CouponPeriod &c : fixed_leg)
            m = std::max(m, c.payment);
        for (const CouponPeriod &c : floating_leg)
            m = std::max(m, c.payment);
        return m;
    }

    InterestRateSwap InterestRateSwap::tail_from(Time t) const
    {
        constexpr Real eps = 1e-10;
        InterestRateSwap tail = *this;
        const auto before = [t](const CouponPeriod &c)
        { return c.start < t - eps; };
        tail.fixed_leg.erase(std::remove_if(tail.fixed_leg.begin(), tail.fixed_leg.end(), before),
                             tail.fixed_leg.end());
        tail.floating_leg.erase(std::remove_if(tail.floating_leg.begin(), tail.floating_leg.end(), before),
                                tail.floating_leg.end());
        return tail;
    }

    InterestRateSwap make_swap(Time start, Time tenor, Real fixed_rate, int fixed_frequency,
                               int float_frequency, Real notional, bool payer, Real spread)
    {
        if (!(start >= 0.0) || !(tenor > 0.0))
            throw InvalidInput("make_swap: start must be >= 0 and tenor > 0");
        InterestRateSwap swap;
        swap.fixed_leg = regular_leg(start, tenor, fixed_frequency, "fixed");
        swap.floating_leg = regular_leg(start, tenor, float_frequency, "floating");
        swap.fixed_rate = fixed_rate;
        swap.spread = spread;
        swap.notional = notional;
        swap.payer = payer;
        return swap;
    }

} // namespace quantModeling
