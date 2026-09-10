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
    }

    TEST(Calendar, UnitedKingdomNewYearSubstitute)
    {
        const auto &c = UnitedKingdom::instance();
        // 1 Jan 2023 was a Sunday -> observed Monday 2 Jan; Tuesday 3 Jan trades.
        EXPECT_TRUE(c.is_holiday(Date(2023, 1, 2)));
        EXPECT_TRUE(c.is_business_day(Date(2023, 1, 3)));
        // 1 Jan 2022 was a Saturday -> observed Monday 3 Jan; Tuesday 4 Jan trades.
        EXPECT_TRUE(c.is_holiday(Date(2022, 1, 3)));
        EXPECT_TRUE(c.is_business_day(Date(2022, 1, 4)));
        // 1 Jan 2021 was a Friday -> the day itself, no substitute.
        EXPECT_TRUE(c.is_holiday(Date(2021, 1, 1)));
        EXPECT_TRUE(c.is_business_day(Date(2021, 1, 4)));
    }

    TEST(Calendar, UnitedKingdomChristmasSubstitute)
    {
        const auto &c = UnitedKingdom::instance();
        // 25 Dec 2021 was a Saturday: Christmas -> Mon 27, Boxing Day -> Tue 28.
        EXPECT_TRUE(c.is_holiday(Date(2021, 12, 27)));
        EXPECT_TRUE(c.is_holiday(Date(2021, 12, 28)));
        EXPECT_TRUE(c.is_business_day(Date(2021, 12, 29)));
        // 25 Dec 2022 was a Sunday: Boxing Day stays Mon 26, Christmas -> Tue 27.
        EXPECT_TRUE(c.is_holiday(Date(2022, 12, 26)));
        EXPECT_TRUE(c.is_holiday(Date(2022, 12, 27)));
        EXPECT_TRUE(c.is_business_day(Date(2022, 12, 28)));
        // 25 Dec 2020 was a Friday: Christmas stays 25, Boxing Day -> Mon 28.
        EXPECT_TRUE(c.is_holiday(Date(2020, 12, 25)));
        EXPECT_TRUE(c.is_holiday(Date(2020, 12, 28)));
        EXPECT_TRUE(c.is_business_day(Date(2020, 12, 29)));
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
