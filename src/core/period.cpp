#include "quantModeling/core/period.hpp"

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace quantModeling
{

    Period Period::parse(const std::string &s)
    {
        if (s.size() < 2)
            throw InvalidInput("Period::parse: expected e.g. \"3M\", got '" + s + "'");

        const char unit_ch = static_cast<char>(std::toupper(s.back()));
        TimeUnit unit;
        switch (unit_ch)
        {
            case 'D':
                unit = TimeUnit::Days;
                break;
            case 'W':
                unit = TimeUnit::Weeks;
                break;
            case 'M':
                unit = TimeUnit::Months;
                break;
            case 'Y':
                unit = TimeUnit::Years;
                break;
            default:
                throw InvalidInput("Period::parse: unknown unit '" +
                                   std::string(1, s.back()) + "' in '" + s + "'");
        }

        int n = 0;
        const char *first = s.data();
        const char *last = s.data() + s.size() - 1;
        const auto [ptr, ec] = std::from_chars(first, last, n);
        if (ec != std::errc{} || ptr != last)
            throw InvalidInput("Period::parse: bad count in '" + s + "'");

        return Period{n, unit};
    }

    std::string Period::to_string() const
    {
        const char u = unit == TimeUnit::Days     ? 'D'
                       : unit == TimeUnit::Weeks  ? 'W'
                       : unit == TimeUnit::Months ? 'M'
                                                  : 'Y';
        return std::to_string(n) + u;
    }

    Date advance(const Date &d, const Period &p)
    {
        using namespace std::chrono;

        switch (p.unit)
        {
            case TimeUnit::Days:
                return d + p.n;
            case TimeUnit::Weeks:
                return d + 7 * p.n;
            case TimeUnit::Months:
            case TimeUnit::Years:
            {
                const auto ymd0 = d.ymd();
                const int months_to_add =
                    p.unit == TimeUnit::Years ? 12 * p.n : p.n;
                const year_month ym =
                    year_month{ymd0.year(), ymd0.month()} + months{months_to_add};
                const year_month_day_last last{ym / std::chrono::last};
                const unsigned dd =
                    std::min<unsigned>(static_cast<unsigned>(ymd0.day()),
                                       static_cast<unsigned>(last.day()));
                return Date(static_cast<int>(ym.year()),
                            static_cast<unsigned>(ym.month()), dd);
            }
        }
        return d; // unreachable
    }

} // namespace quantModeling
