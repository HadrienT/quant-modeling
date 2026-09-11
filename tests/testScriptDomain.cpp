#include <gtest/gtest.h>

#include "quantModeling/scripting/domain.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/domain_processor.hpp"
#include "quantModeling/scripting/visitors/if_processor.hpp"
#include "quantModeling/scripting/visitors/var_indexer.hpp"

#include <string>
#include <vector>

namespace quantModeling::scripting
{
    namespace
    {
        /// Parse `if <cond> then x = 1 endIf` (optionally prefixed by setup
        /// statements) and return the condition node of the last event's `if`.
        const NodeComparison &condition_of(const std::vector<Event> &events)
        {
            const Node &iff = *events.back().statements.at(0)->arguments.at(0);
            return dynamic_cast<const NodeComparison &>(*iff.arguments.at(0));
        }

        std::vector<Event> analysed(const std::string &src, double eps = 0.01)
        {
            std::vector<Event> events = parse_script(src);
            VarIndexer indexer;
            indexer.index(events);
            IfProcessor().process(events);
            DomainProcessor(indexer.count(), eps).process(events);
            return events;
        }
    } // namespace

    // ── Domain arithmetic ───────────────────────────────────────────────────

    TEST(Domain, SingletonArithmetic)
    {
        const Domain a = Domain::singleton(3.0);
        const Domain b = Domain::singleton(5.0);
        EXPECT_TRUE((a + b).contains(8.0));
        EXPECT_TRUE((a - b).contains(-2.0));
        EXPECT_TRUE((a * b).contains(15.0));
        EXPECT_TRUE((a + b).is_discrete());
    }

    TEST(Domain, UnionOfSingletonsStaysDiscrete)
    {
        const Domain flag =
            domain_union(Domain::singleton(0.0), Domain::singleton(1.0));
        EXPECT_TRUE(flag.is_discrete());
        const Domain shifted = flag - Domain::singleton(1.0); // {-1, 0}
        EXPECT_TRUE(shifted.is_discrete());
        EXPECT_TRUE(shifted.contains(0.0));
        EXPECT_TRUE(shifted.contains(-1.0));
        EXPECT_FALSE(shifted.all_negative());     // 0 is in it
        EXPECT_FALSE(shifted.all_non_negative()); // -1 is in it
    }

    TEST(Domain, IntervalsAreNotDiscrete)
    {
        const Domain spot = Domain::greater_than(0.0);
        EXPECT_FALSE(spot.is_discrete());
        EXPECT_TRUE(spot.all_positive());

        const Domain diff = spot - Domain::singleton(100.0); // (-100, +inf)
        EXPECT_FALSE(diff.is_discrete());
        EXPECT_FALSE(diff.all_positive());
        EXPECT_FALSE(diff.all_negative());
        EXPECT_TRUE(diff.contains(0.0));

        const Domain minus_spot = spot - spot; // whole real line
        EXPECT_TRUE(minus_spot.contains(0.0));
        EXPECT_FALSE(minus_spot.all_positive());
    }

    TEST(Domain, MinMaxAbs)
    {
        const Domain r = Domain::real_line();
        EXPECT_TRUE(domain_max(r, Domain::singleton(0.0)).all_non_negative());
        EXPECT_TRUE(domain_abs(r).all_non_negative());
        EXPECT_TRUE(domain_min(Domain::singleton(2.0), Domain::singleton(5.0))
                        .contains(2.0));
    }

    // ── DomainProcessor drives the smoothing decision ───────────────────────

    TEST(DomainProcessor, ConstantConditionsFold)
    {
        EXPECT_TRUE(condition_of(analysed(
                                     "2025-01-01\n    if 1 > 0 then x = 1 endIf\n"))
                        .alwaysTrue);
        EXPECT_TRUE(condition_of(analysed(
                                     "2025-01-01\n    if 2 < 1 then x = 1 endIf\n"))
                        .alwaysFalse);
    }

    TEST(DomainProcessor, SpotIsAlwaysPositive)
    {
        // spot() > 0 can never be false
        EXPECT_TRUE(condition_of(analysed(
                                     "2025-01-01\n    if spot() > 0 then x = 1 endIf\n"))
                        .alwaysTrue);
    }

    TEST(DomainProcessor, FlagTestIsDiscrete)
    {
        // ki takes only 0 and 1 -> `ki = 1` is an isolated test, never smoothed
        const auto events = analysed(
            "2025-01-01\n    ki = 0\n"
            "2025-06-01\n    if spot() < 90 then ki = 1 endIf\n"
            "2025-12-01\n    if ki = 1 then x = 1 endIf\n");
        const NodeComparison &c = condition_of(events);
        EXPECT_TRUE(c.discrete);
        EXPECT_EQ(c.eps, 0.0);
        EXPECT_FALSE(c.alwaysTrue);
        EXPECT_FALSE(c.alwaysFalse);
    }

    TEST(DomainProcessor, MarketTestGetsAnEps)
    {
        // spot() vs a strike straddles zero and is continuous -> smoothed
        const auto events =
            analysed("2025-01-01\n    if spot() > 100 then x = 1 endIf\n", 0.5);
        const NodeComparison &c = condition_of(events);
        EXPECT_FALSE(c.discrete);
        EXPECT_FALSE(c.alwaysTrue);
        EXPECT_FALSE(c.alwaysFalse);
        EXPECT_DOUBLE_EQ(c.eps, 0.5);
    }

    TEST(DomainProcessor, BarrierOnARelativeLevelGetsAnEps)
    {
        const auto events = analysed(
            "2025-01-01\n    s0 = spot()\n"
            "2025-12-01\n    if spot() < 0.70 * s0 then x = 1 endIf\n",
            0.25);
        const NodeComparison &c = condition_of(events);
        EXPECT_FALSE(c.discrete);
        EXPECT_DOUBLE_EQ(c.eps, 0.25);
    }

} // namespace quantModeling::scripting
