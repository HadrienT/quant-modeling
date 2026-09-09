#ifndef QM_MARKET_CALENDARS_HPP
#define QM_MARKET_CALENDARS_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/period.hpp"
#include "quantModeling/market/conventions.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief A holiday calendar: weekend rule plus a set of holidays.
     *
     * Holidays are computed by rule (fixed dates, n-th weekday of a month,
     * Easter-relative), not loaded from a table, so any year is supported.
     * Concrete calendars are immutable singletons reached through instance();
     * pass them by const reference or non-owning pointer.
     *
     * Coverage is desk-grade but not exhaustive: one-off holidays (royal
     * weddings, state funerals, the 9/11 closure) and pre-2000 rule changes
     * are not modelled.
     */
    class Calendar
    {
      public:
        virtual ~Calendar() = default;
        virtual const char *name() const noexcept = 0;

        bool is_weekend(const Date &d) const;
        bool is_business_day(const Date &d) const;
        bool is_holiday(const Date &d) const;

        /// Roll d onto a business day according to the convention.
        Date adjust(const Date &d,
                    BusinessDayConvention c = BusinessDayConvention::Following) const;

        /// Calendar-advance d by the period, then adjust. When end_of_month is
        /// set and d is its month's last day, a Months/Years step lands on the
        /// target month's last business day.
        Date advance(const Date &d, const Period &p,
                     BusinessDayConvention c = BusinessDayConvention::Following,
                     bool end_of_month = false) const;

        /// Business days in the half-open interval (start, end]; negative when
        /// end < start.
        long business_days_between(const Date &start, const Date &end) const;

      protected:
        /// Weekend rule. Default: Saturday and Sunday.
        virtual bool is_weekend_impl(Weekday wd) const;
        /// True if d is a holiday, ignoring the weekend rule.
        virtual bool is_holiday_impl(const Date &d) const = 0;
    };

    /// Weekends only, no holidays.
    class NullCalendar final : public Calendar
    {
      public:
        const char *name() const noexcept override { return "Null"; }
        static const NullCalendar &instance();

      protected:
        bool is_holiday_impl(const Date &) const override { return false; }
    };

    /// TARGET / T2 (the Eurosystem payment-system calendar), 2000 onwards.
    class TARGET final : public Calendar
    {
      public:
        const char *name() const noexcept override { return "TARGET"; }
        static const TARGET &instance();

      protected:
        bool is_holiday_impl(const Date &d) const override;
    };

    /// United States government-securities / SOFR calendar.
    class UnitedStates final : public Calendar
    {
      public:
        const char *name() const noexcept override { return "UnitedStates"; }
        static const UnitedStates &instance();

      protected:
        bool is_holiday_impl(const Date &d) const override;
    };

    /// United Kingdom bank holidays (settlement calendar).
    class UnitedKingdom final : public Calendar
    {
      public:
        const char *name() const noexcept override { return "UnitedKingdom"; }
        static const UnitedKingdom &instance();

      protected:
        bool is_holiday_impl(const Date &d) const override;
    };

    /// Union of several calendars: a day is closed if any member calendar
    /// closes it. Members are borrowed, not owned.
    class JointCalendar final : public Calendar
    {
      public:
        explicit JointCalendar(std::vector<const Calendar *> calendars);
        const char *name() const noexcept override { return "Joint"; }

      protected:
        bool is_holiday_impl(const Date &d) const override;

      private:
        std::vector<const Calendar *> calendars_;
    };

} // namespace quantModeling

#endif // QM_MARKET_CALENDARS_HPP
