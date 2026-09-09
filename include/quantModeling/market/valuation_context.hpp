#ifndef QM_MARKET_VALUATION_CONTEXT_HPP
#define QM_MARKET_VALUATION_CONTEXT_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/calendars.hpp"
#include "quantModeling/market/conventions.hpp"

#include <vector>

namespace quantModeling
{

    /**
     * @brief Anchors calendar dates to the numeric Time axis.
     *
     * Time 0 is the valuation date. t(d) is the year-fraction from the
     * valuation date to d under `basis`. `basis` and `calendar` are borrowed
     * immutable singletons (Actual365Fixed::instance(), TARGET::instance(),
     * ...) — never owned, never wrapped in a shared_ptr.
     *
     * Instruments store Date; engines and adapters call this to obtain the
     * TimeLine the simulation architecture consumes.
     */
    struct ValuationContext
    {
        Date valuation_date;
        const DayCounter *basis = &Actual365Fixed::instance();
        const Calendar *calendar = &NullCalendar::instance();

        Time t(const Date &d) const
        {
            return basis->year_fraction(valuation_date, d);
        }

        std::vector<Time> t(const std::vector<Date> &ds) const
        {
            std::vector<Time> out;
            out.reserve(ds.size());
            for (const Date &d : ds)
                out.push_back(t(d));
            return out;
        }
    };

} // namespace quantModeling

#endif // QM_MARKET_VALUATION_CONTEXT_HPP
