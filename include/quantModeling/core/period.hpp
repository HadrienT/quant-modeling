#ifndef QM_CORE_PERIOD_HPP
#define QM_CORE_PERIOD_HPP

#include "quantModeling/core/date.hpp"

#include <string>

namespace quantModeling
{

    enum class TimeUnit
    {
        Days,
        Weeks,
        Months,
        Years
    };

    /// A tenor: an integer number of calendar units. Negative n steps backwards.
    struct Period
    {
        int n = 0;
        TimeUnit unit = TimeUnit::Days;

        Period() = default;
        Period(int n_, TimeUnit u) : n(n_), unit(u) {}

        /// Parse "3M", "1Y", "2W", "10D" (unit letter case-insensitive).
        static Period parse(const std::string &s);

        std::string to_string() const;
    };

    /**
     * @brief Advance a date by a period with plain calendar arithmetic.
     *
     * No business-day adjustment (use Calendar::advance for that). Month and
     * year steps clamp to the last day of the target month, so
     * 31-Jan-2023 + 1M is 28-Feb-2023.
     */
    Date advance(const Date &d, const Period &p);

    inline Date operator+(const Date &d, const Period &p) { return advance(d, p); }
    inline Date operator-(const Date &d, const Period &p)
    {
        return advance(d, Period{-p.n, p.unit});
    }

} // namespace quantModeling

#endif // QM_CORE_PERIOD_HPP
