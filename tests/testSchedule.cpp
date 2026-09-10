#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/period.hpp"
#include "quantModeling/market/calendars.hpp"
#include "quantModeling/market/schedule.hpp"

namespace quantModeling
{

    TEST(Schedule, QuarterlyBackwardIsAdjustedAndBounded)
    {
        Schedule s(Date(2023, 3, 15), Date(2024, 3, 15), Period::parse("3M"),
                   TARGET::instance(), BusinessDayConvention::ModifiedFollowing,
                   DateGenerationRule::Backward);
        EXPECT_EQ(s.size(), 5u);
        EXPECT_EQ(s.front(), Date(2023, 3, 15));
        EXPECT_EQ(s.back(), Date(2024, 3, 15));
        for (const Date &d : s)
            EXPECT_TRUE(TARGET::instance().is_business_day(d));
    }

    TEST(Schedule, ForwardAndBackwardSamePeriodCount)
    {
        const Date eff(2023, 3, 15), term(2024, 3, 15);
        const Period q = Period::parse("3M");
        Schedule fwd(eff, term, q, TARGET::instance(),
                     BusinessDayConvention::ModifiedFollowing,
                     DateGenerationRule::Forward);
        Schedule bwd(eff, term, q, TARGET::instance(),
                     BusinessDayConvention::ModifiedFollowing,
                     DateGenerationRule::Backward);
        EXPECT_EQ(fwd.size(), bwd.size());
    }

    TEST(Schedule, ThirdWednesdayRuleYieldsImmDates)
    {
        Schedule s(Date(2023, 3, 1), Date(2024, 6, 1), Period::parse("3M"),
                   TARGET::instance(), BusinessDayConvention::Unadjusted,
                   DateGenerationRule::ThirdWednesday);
        EXPECT_FALSE(s.empty());
        for (const Date &d : s)
            EXPECT_TRUE(imm::is_imm_date(d));
    }

    TEST(Schedule, ExplicitListIsSortedUnique)
    {
        Schedule s(std::vector<Date>{Date(2024, 1, 1), Date(2023, 1, 1),
                                     Date(2023, 1, 1)});
        EXPECT_EQ(s.size(), 2u);
        EXPECT_EQ(s.front(), Date(2023, 1, 1));
    }

    TEST(Schedule, RejectsBadInput)
    {
        EXPECT_THROW(Schedule(Date(2024, 1, 1), Date(2023, 1, 1), Period::parse("3M"),
                              NullCalendar::instance()),
                     InvalidInput);
    }

    TEST(Imm, ThirdWednesdayAndNext)
    {
        EXPECT_EQ(imm::third_wednesday(2024, Month::June), Date(2024, 6, 19));
        EXPECT_EQ(imm::third_wednesday(2024, Month::March), Date(2024, 3, 20));
        EXPECT_EQ(imm::next(Date(2024, 1, 1)), Date(2024, 3, 20));
        EXPECT_EQ(imm::next(Date(2024, 3, 20)), Date(2024, 6, 19));
        EXPECT_EQ(imm::next(Date(2024, 3, 20), /*include_ref=*/true),
                  Date(2024, 3, 20));
        EXPECT_TRUE(imm::is_imm_date(Date(2024, 3, 20)));
        EXPECT_FALSE(imm::is_imm_date(Date(2024, 3, 19)));
    }

} // namespace quantModeling
