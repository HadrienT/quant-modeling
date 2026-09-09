#ifndef QM_MARKET_CONVENTIONS_HPP
#define QM_MARKET_CONVENTIONS_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/types.hpp"

namespace quantModeling
{

    enum class BusinessDayConvention
    {
        Following,
        ModifiedFollowing,
        Preceding,
        ModifiedPreceding,
        Unadjusted
    };

    /**
     * @brief Day-count / year-fraction convention.
     *
     * Stateless. Concrete conventions are immutable singletons reached through
     * the static instance() accessors and passed by const reference or
     * non-owning pointer — never wrapped in a shared_ptr.
     */
    class DayCounter
    {
      public:
        virtual ~DayCounter() = default;
        virtual const char *name() const noexcept = 0;
        virtual long day_count(const Date &start, const Date &end) const = 0;
        virtual Time year_fraction(const Date &start, const Date &end) const = 0;
    };

    class Actual360 final : public DayCounter
    {
      public:
        const char *name() const noexcept override { return "Actual/360"; }
        long day_count(const Date &s, const Date &e) const override { return e - s; }
        Time year_fraction(const Date &s, const Date &e) const override
        {
            return static_cast<Time>(e - s) / 360.0;
        }
        static const Actual360 &instance();
    };

    class Actual365Fixed final : public DayCounter
    {
      public:
        const char *name() const noexcept override { return "Actual/365 (Fixed)"; }
        long day_count(const Date &s, const Date &e) const override { return e - s; }
        Time year_fraction(const Date &s, const Date &e) const override
        {
            return static_cast<Time>(e - s) / 365.0;
        }
        static const Actual365Fixed &instance();
    };

    /// 30/360 US "Bond Basis" (30U/360). End-of-February is not treated
    /// specially (that is the NASD variant).
    class Thirty360 final : public DayCounter
    {
      public:
        const char *name() const noexcept override { return "30/360 (Bond Basis)"; }
        long day_count(const Date &s, const Date &e) const override;
        Time year_fraction(const Date &s, const Date &e) const override
        {
            return static_cast<Time>(day_count(s, e)) / 360.0;
        }
        static const Thirty360 &instance();
    };

    /// Actual/Actual (ISDA): actual days apportioned between the leap-year and
    /// non-leap-year parts of the interval.
    class ActualActualISDA final : public DayCounter
    {
      public:
        const char *name() const noexcept override
        {
            return "Actual/Actual (ISDA)";
        }
        long day_count(const Date &s, const Date &e) const override { return e - s; }
        Time year_fraction(const Date &s, const Date &e) const override;
        static const ActualActualISDA &instance();
    };

} // namespace quantModeling

#endif // QM_MARKET_CONVENTIONS_HPP
