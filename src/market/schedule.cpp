#include "quantModeling/market/schedule.hpp"

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace quantModeling
{

    namespace
    {
        void sort_unique(std::vector<Date> &v)
        {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        std::vector<Date> raw_forward(const Date &eff, const Date &term,
                                      const Period &tenor)
        {
            std::vector<Date> out{eff};
            for (int k = 1;; ++k)
            {
                const Date d = advance(eff, Period{tenor.n * k, tenor.unit});
                if (!(d < term))
                    break;
                out.push_back(d);
            }
            out.push_back(term);
            return out;
        }

        std::vector<Date> raw_backward(const Date &eff, const Date &term,
                                       const Period &tenor)
        {
            std::vector<Date> out{term};
            for (int k = 1;; ++k)
            {
                const Date d = advance(term, Period{-tenor.n * k, tenor.unit});
                if (!(eff < d))
                    break;
                out.push_back(d);
            }
            out.push_back(eff);
            std::reverse(out.begin(), out.end());
            return out;
        }
    } // namespace

    Schedule::Schedule(const Date &effective, const Date &termination,
                       const Period &tenor, const Calendar &calendar,
                       BusinessDayConvention convention, DateGenerationRule rule,
                       bool end_of_month)
    {
        if (!(effective < termination))
            throw InvalidInput("Schedule: effective date must precede termination");
        if (rule != DateGenerationRule::Zero && tenor.n <= 0)
            throw InvalidInput("Schedule: tenor must be positive");

        std::vector<Date> raw;
        switch (rule)
        {
            case DateGenerationRule::Forward:
                raw = raw_forward(effective, termination, tenor);
                break;
            case DateGenerationRule::Backward:
                raw = raw_backward(effective, termination, tenor);
                break;
            case DateGenerationRule::Zero:
                raw = {effective, termination};
                break;
            case DateGenerationRule::ThirdWednesday:
                raw = raw_backward(effective, termination, tenor);
                for (Date &d : raw)
                    d = imm::third_wednesday(d.year(), d.month());
                break;
        }

        sort_unique(raw);

        const bool eom = end_of_month && effective.is_end_of_month() &&
                         (tenor.unit == TimeUnit::Months ||
                          tenor.unit == TimeUnit::Years) &&
                         rule != DateGenerationRule::ThirdWednesday;

        dates_.reserve(raw.size());
        for (const Date &d : raw)
        {
            const Date base = eom ? Date::end_of_month(d) : d;
            dates_.push_back(calendar.adjust(base, convention));
        }
        sort_unique(dates_);
    }

    Schedule::Schedule(std::vector<Date> dates)
        : dates_(std::move(dates))
    {
        sort_unique(dates_);
        if (dates_.empty())
            throw InvalidInput("Schedule: empty date list");
    }

    // ── IMM helpers ────────────────────────────────────────────────────────

    namespace imm
    {
        Date third_wednesday(int year, Month month)
        {
            return Date::nth_weekday(year, month, Weekday::Wednesday, 3);
        }

        bool is_imm_date(const Date &d)
        {
            const Month m = d.month();
            if (m != Month::March && m != Month::June && m != Month::September &&
                m != Month::December)
                return false;
            return d == third_wednesday(d.year(), m);
        }

        Date next(const Date &d, bool include_ref)
        {
            constexpr std::array<Month, 4> q{Month::March, Month::June,
                                             Month::September, Month::December};
            int year = d.year();
            for (int guard = 0; guard < 12; ++guard)
            {
                for (const Month m : q)
                {
                    const Date candidate = third_wednesday(year, m);
                    if (d < candidate || (include_ref && candidate == d))
                        return candidate;
                }
                ++year;
            }
            throw InvalidInput("imm::next: no IMM date found (unreachable)");
        }
    } // namespace imm

} // namespace quantModeling
