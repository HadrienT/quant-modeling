#include "quantModeling/core/date.hpp"

#include <array>
#include <charconv>
#include <cstdio>

namespace quantModeling
{

    namespace
    {
        std::chrono::sys_days checked_ymd(int y, unsigned m, unsigned d,
                                          const char *ctx)
        {
            const std::chrono::year_month_day ymd{
                std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
            if (!ymd.ok())
                throw InvalidInput(std::string(ctx) + ": invalid calendar date " +
                                   std::to_string(y) + "-" + std::to_string(m) + "-" +
                                   std::to_string(d));
            return std::chrono::sys_days{ymd};
        }
    } // namespace

    Date::Date(int year, unsigned month, unsigned day)
        : days_(checked_ymd(year, month, day, "Date"))
    {
    }

    Date::Date(int year, Month month, unsigned day)
        : days_(checked_ymd(year, static_cast<unsigned>(month), day, "Date"))
    {
    }

    Date Date::from_iso(const std::string &s)
    {
        // Strict "YYYY-MM-DD": 10 chars, dashes at positions 4 and 7.
        if (s.size() != 10 || s[4] != '-' || s[7] != '-')
            throw InvalidInput("Date::from_iso: expected YYYY-MM-DD, got '" + s + "'");

        auto parse = [&](std::size_t pos, std::size_t len, const char *field) -> int
        {
            int value = 0;
            const char *first = s.data() + pos;
            const char *last = first + len;
            const auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last)
                throw InvalidInput("Date::from_iso: bad " + std::string(field) +
                                   " in '" + s + "'");
            return value;
        };

        const int y = parse(0, 4, "year");
        const int m = parse(5, 2, "month");
        const int d = parse(8, 2, "day");
        return Date(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
    }

    Date Date::nth_weekday(int year, Month month, Weekday wd, unsigned n)
    {
        if (n < 1 || n > 5)
            throw InvalidInput("Date::nth_weekday: n must be in [1, 5]");

        const std::chrono::year_month_weekday ymw{
            std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
            std::chrono::weekday{static_cast<unsigned>(wd)}[n]};
        if (!ymw.ok())
            throw InvalidInput("Date::nth_weekday: no " + std::to_string(n) +
                               "-th weekday in that month");
        return Date(std::chrono::sys_days{ymw});
    }

    int Date::year() const
    {
        return static_cast<int>(ymd().year());
    }

    Month Date::month() const
    {
        return static_cast<Month>(static_cast<unsigned>(ymd().month()));
    }

    unsigned Date::day() const
    {
        return static_cast<unsigned>(ymd().day());
    }

    Weekday Date::weekday() const
    {
        return static_cast<Weekday>(std::chrono::weekday{days_}.c_encoding());
    }

    bool Date::is_end_of_month() const
    {
        const auto this_ymd = ymd();
        const std::chrono::year_month_day_last last{
            this_ymd.year() / this_ymd.month() / std::chrono::last};
        return this_ymd.day() == last.day();
    }

    Date Date::end_of_month(const Date &d)
    {
        const auto y_m = d.ymd();
        const std::chrono::year_month_day_last last{y_m.year() / y_m.month() /
                                                    std::chrono::last};
        return Date(std::chrono::sys_days{last});
    }

    std::string Date::to_iso() const
    {
        const auto y_m_d = ymd();
        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02u",
                      static_cast<int>(y_m_d.year()),
                      static_cast<unsigned>(y_m_d.month()),
                      static_cast<unsigned>(y_m_d.day()));
        return std::string(buf.data());
    }

} // namespace quantModeling
