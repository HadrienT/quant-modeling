#include "quantModeling/market/conventions.hpp"

#include <chrono>

namespace quantModeling
{

    namespace
    {
        int days_in_year(int y)
        {
            return std::chrono::year{y}.is_leap() ? 366 : 365;
        }
    } // namespace

    // ── Actual/360, Actual/365F ──────────────────────────────────────────────

    const Actual360 &Actual360::instance()
    {
        static const Actual360 x;
        return x;
    }

    const Actual365Fixed &Actual365Fixed::instance()
    {
        static const Actual365Fixed x;
        return x;
    }

    // ── 30/360 (Bond Basis) ─────────────────────────────────────────────────

    long Thirty360::day_count(const Date &s, const Date &e) const
    {
        int d1 = static_cast<int>(s.day());
        int d2 = static_cast<int>(e.day());
        const int m1 = static_cast<int>(s.month());
        const int m2 = static_cast<int>(e.month());
        const int y1 = s.year();
        const int y2 = e.year();

        if (d1 == 31)
            d1 = 30;
        if (d2 == 31 && d1 == 30)
            d2 = 30;

        return 360L * (y2 - y1) + 30L * (m2 - m1) + (d2 - d1);
    }

    const Thirty360 &Thirty360::instance()
    {
        static const Thirty360 x;
        return x;
    }

    // ── Actual/Actual (ISDA) ────────────────────────────────────────────────

    Time ActualActualISDA::year_fraction(const Date &s, const Date &e) const
    {
        if (e == s)
            return 0.0;
        if (e < s)
            return -year_fraction(e, s);

        const int y1 = s.year();
        const int y2 = e.year();

        if (y1 == y2)
            return static_cast<Time>(e - s) / days_in_year(y1);

        // Stub from s to the first day of year y1+1.
        const Date start_of_y2{y2, 1u, 1u};
        const Date start_of_y1_plus_1{y1 + 1, 1u, 1u};

        Time yf = static_cast<Time>(start_of_y1_plus_1 - s) / days_in_year(y1);
        yf += static_cast<Time>(y2 - y1 - 1);
        yf += static_cast<Time>(e - start_of_y2) / days_in_year(y2);
        return yf;
    }

    const ActualActualISDA &ActualActualISDA::instance()
    {
        static const ActualActualISDA x;
        return x;
    }

} // namespace quantModeling
