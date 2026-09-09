#ifndef QM_CORE_DATE_HPP
#define QM_CORE_DATE_HPP

#include "quantModeling/core/types.hpp"

#include <chrono>
#include <compare>
#include <string>

namespace quantModeling
{

    enum class Month : unsigned
    {
        January = 1,
        February = 2,
        March = 3,
        April = 4,
        May = 5,
        June = 6,
        July = 7,
        August = 8,
        September = 9,
        October = 10,
        November = 11,
        December = 12
    };

    /// c-encoding: Sunday == 0 ... Saturday == 6 (matches std::chrono::weekday).
    enum class Weekday : unsigned
    {
        Sunday = 0,
        Monday = 1,
        Tuesday = 2,
        Wednesday = 3,
        Thursday = 4,
        Friday = 5,
        Saturday = 6
    };

    /**
     * @brief A calendar date, with no time-of-day and no timezone.
     *
     * Thin value type over std::chrono::sys_days (a day count since the Unix
     * epoch): 4 bytes, trivially copyable, no allocation, defaulted ordering.
     * All the calendar math (leap years, month lengths, weekday of a date,
     * "3rd Wednesday") is delegated to the C++20 <chrono> calendar.
     *
     * This is the type instruments store. It is converted to the numeric
     * Time = double year-fraction used by models and engines through a
     * DayCounter anchored at a valuation date (see ValuationContext).
     */
    class Date
    {
      public:
        Date() = default;
        Date(int year, unsigned month, unsigned day);
        Date(int year, Month month, unsigned day);
        explicit Date(std::chrono::sys_days d) : days_(d) {}

        /// Parse ISO-8601 extended form "YYYY-MM-DD".
        /// @throws InvalidInput on a malformed string or an impossible date
        ///         (e.g. "2023-02-29").
        static Date from_iso(const std::string &s);

        /// n-th occurrence of a weekday in a given month (n = 1..5).
        /// nth_weekday(2024, Month::March, Weekday::Wednesday, 3) is the 3rd
        /// Wednesday of March 2024. @throws InvalidInput if that occurrence does
        /// not exist (e.g. a 5th Friday in a month that has only four).
        static Date nth_weekday(int year, Month month, Weekday wd, unsigned n);

        int year() const;
        Month month() const;
        unsigned day() const;
        Weekday weekday() const;

        /// Days since 1970-01-01 (negative before it). Stable sort/hash key.
        int serial() const
        {
            return static_cast<int>(days_.time_since_epoch().count());
        }

        std::chrono::sys_days sys_days() const { return days_; }
        std::chrono::year_month_day ymd() const
        {
            return std::chrono::year_month_day{days_};
        }

        bool is_end_of_month() const;

        /// Last calendar day of d's month.
        static Date end_of_month(const Date &d);

        std::string to_iso() const;

        Date &operator+=(int days)
        {
            days_ += std::chrono::days{days};
            return *this;
        }
        Date &operator-=(int days)
        {
            days_ -= std::chrono::days{days};
            return *this;
        }

        friend Date operator+(Date d, int days) { return d += days; }
        friend Date operator-(Date d, int days) { return d -= days; }

        /// Calendar days from b to a (a - b). Positive when a is later.
        friend int operator-(const Date &a, const Date &b)
        {
            return a.serial() - b.serial();
        }

        friend auto operator<=>(const Date &, const Date &) = default;
        friend bool operator==(const Date &, const Date &) = default;

      private:
        std::chrono::sys_days days_{};
    };

} // namespace quantModeling

#endif // QM_CORE_DATE_HPP
