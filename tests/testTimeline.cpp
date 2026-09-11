#include <gtest/gtest.h>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/timegrid.hpp"

#include <algorithm>
#include <cmath>

namespace quantModeling
{

    TEST(TimeLine, CanonicalSortsAndDedups)
    {
        const TimeLine t = canonical_timeline({1.0, 0.5, 0.5, 0.25, 1.0});
        ASSERT_EQ(t.size(), 3u);
        EXPECT_DOUBLE_EQ(t[0], 0.25);
        EXPECT_DOUBLE_EQ(t[1], 0.5);
        EXPECT_DOUBLE_EQ(t[2], 1.0);
    }

    TEST(TimeLine, MergeIsUnionSortedUnique)
    {
        const TimeLine m = merge_timelines({0.5, 1.0, 2.0}, {0.25, 1.0, 3.0});
        ASSERT_EQ(m.size(), 5u);
        EXPECT_DOUBLE_EQ(m.front(), 0.25);
        EXPECT_DOUBLE_EQ(m.back(), 3.0);
        // Idempotence.
        EXPECT_EQ(merge_timelines(m, m), m);
    }

    TEST(TimeLine, MonitoringStepsRespectMaxDtAndKeepEvents)
    {
        const TimeLine g = add_monitoring_steps({0.5, 1.0}, 0.1);
        ASSERT_FALSE(g.empty());
        // Events preserved.
        EXPECT_TRUE(std::any_of(g.begin(), g.end(),
                                [](Time t)
                                { return std::fabs(t - 0.5) < 1e-12; }));
        EXPECT_NEAR(g.back(), 1.0, 1e-12);
        for (std::size_t i = 1; i < g.size(); ++i)
            EXPECT_LE(g[i] - g[i - 1], 0.1 + 1e-9);
    }

    TEST(TimeLine, EventIndicesLocateEventsInSimGrid)
    {
        const TimeLine events{0.25, 1.0};
        const TimeLine sim = add_monitoring_steps(events, 0.1);
        const auto idx = event_indices(sim, events);
        ASSERT_EQ(idx.size(), 2u);
        EXPECT_NEAR(sim[idx[0]], 0.25, 1e-12);
        EXPECT_NEAR(sim[idx[1]], 1.0, 1e-12);
    }

    TEST(TimeLine, EventIndicesThrowsWhenEventMissing)
    {
        EXPECT_THROW(event_indices({0.1, 0.2, 0.3}, {0.25}), InvalidInput);
    }

    TEST(Sample, AllocateAndInitialize)
    {
        std::vector<SampleDef> defline(2);
        defline[0].discount_mats = {1.0, 2.0};
        defline[1].forward_mats = {1.5};

        Scenario<Real> scen;
        allocate_scenario(scen, defline, /*n_underlyings=*/3);
        ASSERT_EQ(scen.size(), 2u);
        EXPECT_EQ(scen[0].spots.size(), 3u);
        EXPECT_EQ(scen[0].discounts.size(), 2u);
        EXPECT_EQ(scen[1].forwards.size(), 1u);

        scen[0].initialize();
        EXPECT_DOUBLE_EQ(scen[0].numeraire, 1.0);
        for (Real d : scen[0].discounts)
            EXPECT_DOUBLE_EQ(d, 1.0);
    }

} // namespace quantModeling
