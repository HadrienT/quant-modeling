#include "quantModeling/core/timegrid.hpp"

#include "quantModeling/core/types.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    TimeLine canonical_timeline(TimeLine t)
    {
        std::sort(t.begin(), t.end());
        TimeLine out;
        out.reserve(t.size());
        for (const Time x : t)
        {
            if (out.empty() || x - out.back() > TIMELINE_EPS)
                out.push_back(x);
        }
        return out;
    }

    TimeLine merge_timelines(const TimeLine &a, const TimeLine &b)
    {
        TimeLine both;
        both.reserve(a.size() + b.size());
        both.insert(both.end(), a.begin(), a.end());
        both.insert(both.end(), b.begin(), b.end());
        return canonical_timeline(std::move(both));
    }

    TimeLine add_monitoring_steps(const TimeLine &events, Time max_dt)
    {
        const TimeLine base = canonical_timeline(events);
        if (max_dt <= 0.0)
            return base;

        TimeLine out;
        Time prev = 0.0;
        for (const Time t : base)
        {
            if (t <= TIMELINE_EPS)
            {
                prev = t;
                continue;
            }
            const Time gap = t - prev;
            if (gap > max_dt)
            {
                const int n = static_cast<int>(std::ceil(gap / max_dt));
                const Time step = gap / static_cast<Time>(n);
                for (int i = 1; i < n; ++i)
                    out.push_back(prev + static_cast<Time>(i) * step);
            }
            out.push_back(t);
            prev = t;
        }
        return canonical_timeline(std::move(out));
    }

    TimeLine refine_steps(const TimeLine &grid, int r)
    {
        if (r < 1)
            throw InvalidInput("refine_steps: the refinement must be >= 1");
        if (r == 1)
            return grid;
        TimeLine out;
        out.reserve(grid.size() * static_cast<std::size_t>(r));
        Time prev = 0.0;
        for (const Time t : grid)
        {
            const Time step = (t - prev) / static_cast<Time>(r);
            for (int i = 1; i < r; ++i)
                out.push_back(prev + static_cast<Time>(i) * step);
            out.push_back(t);
            prev = t;
        }
        return out;
    }

    std::vector<std::size_t> event_indices(const TimeLine &sim,
                                           const TimeLine &events)
    {
        std::vector<std::size_t> idx;
        idx.reserve(events.size());
        std::size_t j = 0;
        for (const Time e : events)
        {
            while (j < sim.size() && sim[j] < e - TIMELINE_EPS)
                ++j;
            // Steps shorter than TIMELINE_EPS (fine multilevel grids): the
            // closest point, not the first one within the tolerance.
            while (j + 1 < sim.size() && std::fabs(sim[j + 1] - e) < std::fabs(sim[j] - e))
                ++j;
            if (j >= sim.size() || std::fabs(sim[j] - e) > TIMELINE_EPS)
                throw InvalidInput(
                    "event_indices: simulation timeline is missing an event time");
            idx.push_back(j);
        }
        return idx;
    }

    std::vector<std::ptrdiff_t> event_index_of_step(const TimeLine &sim,
                                                    const TimeLine &events)
    {
        const std::vector<std::size_t> idx = event_indices(sim, events);
        std::vector<std::ptrdiff_t> of_step(sim.size(), -1);
        for (std::size_t j = 0; j < idx.size(); ++j)
            of_step[idx[j]] = static_cast<std::ptrdiff_t>(j);
        return of_step;
    }

} // namespace quantModeling
