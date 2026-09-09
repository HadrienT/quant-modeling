#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/market/conventions.hpp"

#include <cmath>

namespace quantModeling
{

    TEST(DayCounter, ZeroLengthIsZero)
    {
        const Date d(2023, 6, 15);
        EXPECT_DOUBLE_EQ(Actual365Fixed::instance().year_fraction(d, d), 0.0);
        EXPECT_DOUBLE_EQ(Actual360::instance().year_fraction(d, d), 0.0);
        EXPECT_DOUBLE_EQ(ActualActualISDA::instance().year_fraction(d, d), 0.0);
    }

    TEST(DayCounter, ActualBasics)
    {
        EXPECT_NEAR(Actual365Fixed::instance().year_fraction(Date(2023, 1, 1),
                                                             Date(2024, 1, 1)),
                    1.0, 1e-12);
        EXPECT_NEAR(Actual360::instance().year_fraction(Date(2023, 1, 1),
                                                        Date(2023, 1, 31)),
                    30.0 / 360.0, 1e-12);
    }

    TEST(DayCounter, Thirty360)
    {
        EXPECT_EQ(Thirty360::instance().day_count(Date(2023, 1, 15), Date(2023, 3, 15)),
                  60);
        // Day 31 collapses to 30.
        EXPECT_EQ(Thirty360::instance().day_count(Date(2023, 1, 31), Date(2023, 7, 31)),
                  180);
    }

    TEST(DayCounter, ActActISDAAdditivity)
    {
        const auto &aa = ActualActualISDA::instance();
        const double whole = aa.year_fraction(Date(2023, 6, 1), Date(2024, 6, 1));
        const double p1 = aa.year_fraction(Date(2023, 6, 1), Date(2024, 1, 1));
        const double p2 = aa.year_fraction(Date(2024, 1, 1), Date(2024, 6, 1));
        EXPECT_NEAR(whole, p1 + p2, 1e-12);
        EXPECT_NEAR(whole, 1.0, 0.01);
    }

    TEST(DayCounter, ActActISDASignSymmetry)
    {
        const auto &aa = ActualActualISDA::instance();
        EXPECT_NEAR(aa.year_fraction(Date(2023, 3, 1), Date(2024, 9, 1)),
                    -aa.year_fraction(Date(2024, 9, 1), Date(2023, 3, 1)), 1e-12);
    }

} // namespace quantModeling
