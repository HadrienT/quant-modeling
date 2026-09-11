#include "quantModeling/market/calendars.hpp"

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <utility>

namespace quantModeling
{

    namespace
    {
        /// Meeus / Jones / Butcher anonymous-Gregorian algorithm: Easter Sunday.
        Date easter_sunday(int y)
        {
            const int a = y % 19;
            const int b = y / 100;
            const int c = y % 100;
            const int d = b / 4;
            const int e = b % 4;
            const int f = (b + 8) / 25;
            const int g = (b - f + 1) / 3;
            const int h = (19 * a + b - d - g + 15) % 30;
            const int i = c / 4;
            const int k = c % 4;
            const int l = (32 + 2 * e + 2 * i - h - k) % 7;
            const int m = (a + 11 * h + 22 * l) / 451;
            const int month = (h + l - 7 * m + 114) / 31; // 3 = March, 4 = April
            const int day = ((h + l - 7 * m + 114) % 31) + 1;
            return Date(y, static_cast<unsigned>(month), static_cast<unsigned>(day));
        }

        Date good_friday(int y)
        {
            return easter_sunday(y) - 2;
        }
        Date easter_monday(int y)
        {
            return easter_sunday(y) + 1;
        }

        /// Observed date of a fixed US holiday: Saturday -> preceding Friday,
        /// Sunday -> following Monday.
        bool is_us_observed(const Date &d, Month month, unsigned day)
        {
            Date h(d.year(), month, day);
            switch (h.weekday())
            {
                case Weekday::Saturday:
                    h -= 1;
                    break;
                case Weekday::Sunday:
                    h += 1;
                    break;
                default:
                    break;
            }
            return d == h;
        }

        /// New Year's Day observed: a weekend New Year rolls to the next
        /// weekday (Saturday -> Monday, Sunday -> Monday).
        Date uk_new_year_observed(int y)
        {
            Date h(y, Month::January, 1);
            if (h.weekday() == Weekday::Saturday)
                h += 2;
            else if (h.weekday() == Weekday::Sunday)
                h += 1;
            return h;
        }

        /// Christmas Day / Boxing Day substitute: when the fixed date lands on
        /// a weekend the bank holiday moves two days on, so a weekend Christmas
        /// + Boxing pair becomes the following Monday and Tuesday.
        Date uk_substitute_bank_holiday(int y, Month month, unsigned day)
        {
            Date h(y, month, day);
            if (h.weekday() == Weekday::Saturday ||
                h.weekday() == Weekday::Sunday)
                h += 2;
            return h;
        }
    } // namespace

    // ── Calendar (base) ─────────────────────────────────────────────────────

    bool Calendar::is_weekend_impl(Weekday wd) const
    {
        return wd == Weekday::Saturday || wd == Weekday::Sunday;
    }

    bool Calendar::is_weekend(const Date &d) const
    {
        return is_weekend_impl(d.weekday());
    }

    bool Calendar::is_business_day(const Date &d) const
    {
        return !is_weekend(d) && !is_holiday_impl(d);
    }

    bool Calendar::is_holiday(const Date &d) const
    {
        return !is_business_day(d);
    }

    Date Calendar::adjust(const Date &d, BusinessDayConvention c) const
    {
        if (c == BusinessDayConvention::Unadjusted || is_business_day(d))
            return d;

        if (c == BusinessDayConvention::Following ||
            c == BusinessDayConvention::ModifiedFollowing)
        {
            Date x = d;
            while (!is_business_day(x))
                x += 1;
            if (c == BusinessDayConvention::ModifiedFollowing &&
                x.month() != d.month())
            {
                x = d;
                while (!is_business_day(x))
                    x -= 1;
            }
            return x;
        }

        // Preceding / ModifiedPreceding
        Date x = d;
        while (!is_business_day(x))
            x -= 1;
        if (c == BusinessDayConvention::ModifiedPreceding && x.month() != d.month())
        {
            x = d;
            while (!is_business_day(x))
                x += 1;
        }
        return x;
    }

    Date Calendar::advance(const Date &d, const Period &p, BusinessDayConvention c,
                           bool end_of_month) const
    {
        Date target = quantModeling::advance(d, p);

        const bool month_or_year =
            p.unit == TimeUnit::Months || p.unit == TimeUnit::Years;
        if (end_of_month && month_or_year && d.is_end_of_month())
            return adjust(Date::end_of_month(target),
                          BusinessDayConvention::Preceding);

        return adjust(target, c);
    }

    long Calendar::business_days_between(const Date &start, const Date &end) const
    {
        if (end == start)
            return 0;
        if (end < start)
            return -business_days_between(end, start);

        long count = 0;
        for (Date d = start + 1; !(end < d); d += 1)
            if (is_business_day(d))
                ++count;
        return count;
    }

    // ── NullCalendar ───────────────────────────────────────────────────────

    const NullCalendar &NullCalendar::instance()
    {
        static const NullCalendar x;
        return x;
    }

    // ── TARGET ─────────────────────────────────────────────────────────────

    bool TARGET::is_holiday_impl(const Date &d) const
    {
        const Month m = d.month();
        const unsigned dd = d.day();
        const int y = d.year();

        if (m == Month::January && dd == 1)
            return true; // New Year's Day
        if (m == Month::May && dd == 1)
            return true; // Labour Day
        if (m == Month::December && (dd == 25 || dd == 26))
            return true; // Christmas, 26 December
        if (d == good_friday(y) || d == easter_monday(y))
            return true;
        return false;
    }

    const TARGET &TARGET::instance()
    {
        static const TARGET x;
        return x;
    }

    // ── UnitedStates (government securities / SOFR) ─────────────────────────

    bool UnitedStates::is_holiday_impl(const Date &d) const
    {
        const Month m = d.month();
        const unsigned dd = d.day();
        const Weekday wd = d.weekday();
        const int y = d.year();

        // New Year's Day (observed); a Friday 31 December covers a Saturday 1 Jan.
        if (is_us_observed(d, Month::January, 1))
            return true;
        if (m == Month::December && dd == 31 && wd == Weekday::Friday)
            return true;

        // Martin Luther King's birthday: 3rd Monday of January (from 1983).
        if (y >= 1983 && m == Month::January && wd == Weekday::Monday && dd >= 15 &&
            dd <= 21)
            return true;
        // Washington's birthday: 3rd Monday of February.
        if (m == Month::February && wd == Weekday::Monday && dd >= 15 && dd <= 21)
            return true;
        // Memorial Day: last Monday of May.
        if (m == Month::May && wd == Weekday::Monday && dd >= 25)
            return true;
        // Juneteenth (observed), from 2021.
        if (y >= 2021 && is_us_observed(d, Month::June, 19))
            return true;
        // Independence Day (observed).
        if (is_us_observed(d, Month::July, 4))
            return true;
        // Labor Day: 1st Monday of September.
        if (m == Month::September && wd == Weekday::Monday && dd <= 7)
            return true;
        // Columbus Day: 2nd Monday of October.
        if (m == Month::October && wd == Weekday::Monday && dd >= 8 && dd <= 14)
            return true;
        // Veterans Day (observed).
        if (is_us_observed(d, Month::November, 11))
            return true;
        // Thanksgiving: 4th Thursday of November.
        if (m == Month::November && wd == Weekday::Thursday && dd >= 22 && dd <= 28)
            return true;
        // Christmas (observed).
        if (is_us_observed(d, Month::December, 25))
            return true;

        return false;
    }

    const UnitedStates &UnitedStates::instance()
    {
        static const UnitedStates x;
        return x;
    }

    // ── UnitedKingdom (bank holidays) ──────────────────────────────────────

    bool UnitedKingdom::is_holiday_impl(const Date &d) const
    {
        const Month m = d.month();
        const unsigned dd = d.day();
        const Weekday wd = d.weekday();
        const int y = d.year();

        if (d == uk_new_year_observed(y))
            return true; // New Year's Day
        if (d == good_friday(y) || d == easter_monday(y))
            return true;
        // Early May Bank Holiday: first Monday of May.
        if (m == Month::May && wd == Weekday::Monday && dd <= 7)
            return true;
        // Spring Bank Holiday: last Monday of May.
        if (m == Month::May && wd == Weekday::Monday && dd >= 25)
            return true;
        // Summer Bank Holiday: last Monday of August.
        if (m == Month::August && wd == Weekday::Monday && dd >= 25)
            return true;
        // Christmas Day and Boxing Day (weekend hits move to Mon / Tue).
        if (d == uk_substitute_bank_holiday(y, Month::December, 25) ||
            d == uk_substitute_bank_holiday(y, Month::December, 26))
            return true;

        return false;
    }

    const UnitedKingdom &UnitedKingdom::instance()
    {
        static const UnitedKingdom x;
        return x;
    }

    // ── JointCalendar ──────────────────────────────────────────────────────

    JointCalendar::JointCalendar(std::vector<const Calendar *> calendars)
        : calendars_(std::move(calendars))
    {
        if (calendars_.empty())
            throw InvalidInput("JointCalendar: needs at least one calendar");
    }

    bool JointCalendar::is_holiday_impl(const Date &d) const
    {
        return std::any_of(calendars_.begin(), calendars_.end(),
                           [&](const Calendar *c)
                           { return c->is_holiday(d); });
    }

} // namespace quantModeling
