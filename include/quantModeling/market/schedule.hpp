#ifndef QM_MARKET_SCHEDULE_HPP
#define QM_MARKET_SCHEDULE_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/period.hpp"
#include "quantModeling/market/calendars.hpp"
#include "quantModeling/market/conventions.hpp"

#include <cstddef>
#include <vector>

namespace quantModeling
{

    enum class DateGenerationRule
    {
        Forward,        ///< generate from the effective date forward
        Backward,       ///< generate from the termination date backward
        ThirdWednesday, ///< roll every generated date to the 3rd Wednesday (IMM)
        Zero            ///< just {effective, termination}
    };

    /**
     * @brief An ordered, business-day-adjusted sequence of dates.
     *
     * Used for coupon periods and observation schedules. Dates are unique and
     * sorted after adjustment.
     */
    class Schedule
    {
      public:
        Schedule(
            const Date &effective, const Date &termination, const Period &tenor,
            const Calendar &calendar,
            BusinessDayConvention convention = BusinessDayConvention::ModifiedFollowing,
            DateGenerationRule rule = DateGenerationRule::Backward,
            bool end_of_month = false);

        /// Wrap an explicit, already-adjusted list (sorted and de-duplicated).
        explicit Schedule(std::vector<Date> dates);

        const std::vector<Date> &dates() const { return dates_; }
        std::size_t size() const { return dates_.size(); }
        bool empty() const { return dates_.empty(); }
        const Date &operator[](std::size_t i) const { return dates_[i]; }
        const Date &front() const { return dates_.front(); }
        const Date &back() const { return dates_.back(); }

        std::vector<Date>::const_iterator begin() const { return dates_.begin(); }
        std::vector<Date>::const_iterator end() const { return dates_.end(); }

      private:
        std::vector<Date> dates_;
    };

    /// IMM date helpers: the 3rd Wednesday of March, June, September, December.
    namespace imm
    {
        Date third_wednesday(int year, Month month);
        bool is_imm_date(const Date &d);
        /// First IMM date after d (or on d when include_ref is set).
        Date next(const Date &d, bool include_ref = false);
    } // namespace imm

} // namespace quantModeling

#endif // QM_MARKET_SCHEDULE_HPP
