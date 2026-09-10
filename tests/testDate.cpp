#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/period.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/valuation_context.hpp"

#include <type_traits>

namespace quantModeling
{

    TEST(Date, TodayIsAfterTheProjectStartAndBeforeFarFuture)
    {
        const Date now = Date::today();
        EXPECT_GT(now - Date::from_iso("2024-01-01"), 0);
        EXPECT_LT(now - Date::from_iso("2100-01-01"), 0);
        // idempotent within a run, and a real weekday
        EXPECT_EQ(Date::today(), now);
        EXPECT_LE(static_cast<unsigned>(now.weekday()), 6u);
    }

    TEST(ValuationContext, AnchorsTimeZeroToTheGivenDate)
    {
        const ValuationContext ctx{Date::from_iso("2024-06-03")};
        EXPECT_DOUBLE_EQ(ctx.t(Date::from_iso("2024-06-03")), 0.0);
        EXPECT_NEAR(ctx.t(Date::from_iso("2025-06-03")), 1.0, 1e-9);
        static_assert(!std::is_default_constructible_v<ValuationContext>,
                      "a ValuationContext must be given a valuation date");
    }

    TEST(Date, IsoRoundTrip)
    {
        const Date d = Date::from_iso("2023-04-07");
        EXPECT_EQ(d.year(), 2023);
        EXPECT_EQ(d.month(), Month::April);
        EXPECT_EQ(d.day(), 7u);
        EXPECT_EQ(d.weekday(), Weekday::Friday);
        EXPECT_EQ(d.to_iso(), "2023-04-07");
    }

    TEST(Date, RejectsImpossibleDate)
    {
        EXPECT_THROW(Date::from_iso("2023-02-29"), InvalidInput);
        EXPECT_THROW(Date(2023, 13, 1), InvalidInput);
        EXPECT_THROW(Date::from_iso("2023/04/07"), InvalidInput);
    }

    TEST(Date, Arithmetic)
    {
        EXPECT_EQ(Date(2024, 2, 29) - Date(2024, 2, 28), 1);
        EXPECT_EQ(Date(2023, 1, 1) + 365, Date(2024, 1, 1));
        EXPECT_LT(Date(2023, 1, 1), Date(2023, 1, 2));
    }

    TEST(Date, EndOfMonth)
    {
        EXPECT_TRUE(Date(2023, 1, 31).is_end_of_month());
        EXPECT_FALSE(Date(2024, 2, 28).is_end_of_month());
        EXPECT_TRUE(Date(2024, 2, 29).is_end_of_month());
        EXPECT_EQ(Date::end_of_month(Date(2024, 2, 10)), Date(2024, 2, 29));
    }

    TEST(Date, NthWeekday)
    {
        // 3rd Wednesday of March 2024 is the 20th.
        EXPECT_EQ(Date::nth_weekday(2024, Month::March, Weekday::Wednesday, 3),
                  Date(2024, 3, 20));
        // No 5th Friday in February 2023.
        EXPECT_THROW(Date::nth_weekday(2023, Month::February, Weekday::Friday, 5),
                     InvalidInput);
    }

    TEST(Period, Parse)
    {
        EXPECT_EQ(Period::parse("3M").n, 3);
        EXPECT_EQ(Period::parse("3M").unit, TimeUnit::Months);
        EXPECT_EQ(Period::parse("1y").unit, TimeUnit::Years);
        EXPECT_EQ(Period::parse("10D").to_string(), "10D");
        EXPECT_THROW(Period::parse("3X"), InvalidInput);
    }

    TEST(Period, AdvanceClampsToMonthEnd)
    {
        EXPECT_EQ(advance(Date(2023, 1, 31), Period::parse("1M")), Date(2023, 2, 28));
        EXPECT_EQ(advance(Date(2024, 1, 31), Period::parse("1M")), Date(2024, 2, 29));
        EXPECT_EQ(advance(Date(2023, 3, 15), Period::parse("1Y")), Date(2024, 3, 15));
        EXPECT_EQ(advance(Date(2023, 3, 15), Period{-1, TimeUnit::Months}),
                  Date(2023, 2, 15));
        EXPECT_EQ(Date(2023, 3, 15) + Period::parse("2W"), Date(2023, 3, 29));
    }

} // namespace quantModeling
