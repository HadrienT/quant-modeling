#ifndef QM_CORE_TIMEGRID_HPP
#define QM_CORE_TIMEGRID_HPP

#include "quantModeling/core/types.hpp"

#include <vector>

namespace quantModeling
{

    /// A sorted, de-duplicated list of times (year-fractions from the
    /// valuation date). This is the Savine "timeline": a product publishes the
    /// dates at which it observes the market; a model merges it with its own
    /// discretisation steps and simulates along the union.
    using TimeLine = std::vector<Time>;

    /// Two times closer than this are treated as coincident when merging
    /// timelines (about one hour and one minute of a 365-day year).
    inline constexpr Time TIMELINE_EPS = 1.0 / (365.0 * 24.0);

    /// Sort, and collapse points within TIMELINE_EPS of each other.
    TimeLine canonical_timeline(TimeLine t);

    /// Union of two timelines, sorted and de-duplicated (within TIMELINE_EPS).
    TimeLine merge_timelines(const TimeLine &a, const TimeLine &b);

    /**
     * @brief Insert intermediate points so no gap in `events` exceeds max_dt.
     *
     * Used by Euler-discretised models (local vol) that need a fine grid
     * between the (possibly sparse) product event dates. Every original event
     * time is preserved. A non-positive max_dt returns the canonical events
     * unchanged. Points at or before 0 are dropped.
     */
    TimeLine add_monitoring_steps(const TimeLine &events, Time max_dt);

    /// Index, into `sim`, of the entry equal (within TIMELINE_EPS) to each
    /// entry of `events`. `sim` must be canonical and contain every event.
    std::vector<std::size_t> event_indices(const TimeLine &sim,
                                           const TimeLine &events);

} // namespace quantModeling

#endif // QM_CORE_TIMEGRID_HPP
