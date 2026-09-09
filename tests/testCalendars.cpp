#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/market/calendars.hpp"

namespace quantModeling
{

    TEST(Calendar, TargetHolidays2023)
    {
        const auto &c = TARGET::instance();
        EXPECT_TRUE(c.is_holiday(Date(2023, 1, 1)));   // New Year
        EXPECT_TRUE(c.is_holiday(Date(2023, 4, 7)));   // Good Friday
        EXPECT_TRUE(c.is_holiday(Date(2023, 4, 10)));  // Easter Monday
        EXPECT_TRUE(c.is_holiday(Date(2023, 5, 1)));   // Labour Day
        EXPECT_TRUE(c.is_holiday(Date(2023, 12, 25))); // Christmas
        EXPECT_TRUE(c.is_holiday(Date(2023, 12, 26))); // 26 December
        EXPECT_TRUE(c.is_business_day(Date(2023, 4, 6)));
        EXPECT_TRUE(c.is_business_day(Date(2023, 7, 4)));
    }

    TEST(Calendar, UnitedStatesHolidays)
    {
        const auto &c = UnitedStates::instance();
        EXPECT_TRUE(c.is_holiday(Date(2023, 1, 16)));  // MLK (3rd Mon Jan)
        EXPECT_TRUE(c.is_holiday(Date(2023, 5, 29)));  // Memorial Day (last Mon May)
        EXPECT_TRUE(c.is_holiday(Date(2023, 6, 19)));  // Juneteenth
        EXPECT_TRUE(c.is_holiday(Date(2023, 7, 4)));   // Independence Day
        EXPECT_TRUE(c.is_holiday(Date(2023, 9, 4)));   // Labor Day (1st Mon Sep)
        EXPECT_TRUE(c.is_holiday(Date(2023, 11, 23))); // Thanksgiving (4th Thu)
        // 4 Jul 2021 was a Sunday -> observed Monday 5 Jul.
        EXPECT_TRUE(c.is_holiday(Date(2021, 7, 5)));
    }

    TEST(Calendar, UnitedKingdomHolidays)
    {
        const auto &c = UnitedKingdom::instance();
        EXPECT_TRUE(c.is_holiday(Date(2023, 4, 7)));  // Good Friday
        EXPECT_TRUE(c.is_holiday(Date(2023, 4, 10))); // Easter Monday
        EXPECT_TRUE(c.is_holiday(Date(2023, 5, 1)));  // Early May BH
        EXPECT_TRUE(c.is_holiday(Date(2023, 5, 29))); // Spring BH
        EXPECT_TRUE(c.is_holiday(Date(2023, 8, 28))); // Summer BH
        // 1 Jan 2023 was a Sunday -> observed Monday 2 Jan.
        EXPECT_TRUE(c.is_holiday(Date(2023, 1, 2)));
    }

    TEST(Calendar, AdjustModifiedFollowingStaysInMonth)
    {
        const auto &c = UnitedKingdom::instance();
        // 29 Apr 2023 is Saturday; 1 May is a bank holiday.
        EXPECT_EQ(c.adjust(Date(2023, 4, 29), BusinessDayConvention::Following),
                  Date(2023, 5, 2));
        EXPECT_EQ(c.adjust(Date(2023, 4, 29),
                           BusinessDayConvention::ModifiedFollowing),
                  Date(2023, 4, 28));
    }

    TEST(Calendar, AdjustIdempotent)
    {
        const auto &c = TARGET::instance();
        const Date a = c.adjust(Date(2023, 4, 7));
        EXPECT_EQ(c.adjust(a), a);
        EXPECT_TRUE(c.is_business_day(a));
    }

    TEST(Calendar, BusinessDaysBetweenCleanWeek)
    {
        // (Sun 2 Apr, Fri 7 Apr] over a holiday-free calendar = Mon..Fri = 5.
        EXPECT_EQ(NullCalendar::instance().business_days_between(Date(2023, 4, 2),
                                                                Date(2023, 4, 7)),
                  5);
        EXPECT_EQ(NullCalendar::instance().business_days_between(Date(2023, 4, 7),
                                                                Date(2023, 4, 2)),
                  -5);
    }

    TEST(Calendar, JointCalendarIsUnion)
    {
        const JointCalendar joint({&TARGET::instance(), &UnitedStates::instance()});
        EXPECT_TRUE(joint.is_holiday(Date(2023, 7, 4)));  // US only
        EXPECT_TRUE(joint.is_holiday(Date(2023, 5, 1)));  // TARGET only
        EXPECT_TRUE(joint.is_business_day(Date(2023, 3, 1)));
    }

} // namespace quantModeling
