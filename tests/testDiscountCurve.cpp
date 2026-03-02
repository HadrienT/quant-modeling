#include <gtest/gtest.h>

#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/core/types.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────────
    //  Flat rate discount curve
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DiscountCurve, FlatRateAtZero)
    {
        DiscountCurve dc(0.05);
        EXPECT_DOUBLE_EQ(dc.discount(0.0), 1.0);
    }

    TEST(DiscountCurve, FlatRateOneYear)
    {
        const Real rate = 0.05;
        DiscountCurve dc(rate);
        EXPECT_NEAR(dc.discount(1.0), std::exp(-rate), 1e-15);
    }

    TEST(DiscountCurve, FlatRateNegativeTime)
    {
        DiscountCurve dc(0.05);
        EXPECT_DOUBLE_EQ(dc.discount(-1.0), 1.0);
    }

    TEST(DiscountCurve, FlatRateMonotoneDecreasing)
    {
        DiscountCurve dc(0.05);
        const Real d1 = dc.discount(1.0);
        const Real d2 = dc.discount(2.0);
        const Real d5 = dc.discount(5.0);
        EXPECT_GT(d1, d2);
        EXPECT_GT(d2, d5);
    }

    TEST(DiscountCurve, FlatRateZeroRate)
    {
        DiscountCurve dc(0.0);
        EXPECT_DOUBLE_EQ(dc.discount(5.0), 1.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Custom discount curve (log-linear interpolation)
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DiscountCurve, CustomCurveExactPoints)
    {
        // At exact grid points, DF should be returned exactly
        std::vector<Time> times = {0.5, 1.0, 2.0, 5.0};
        std::vector<Real> dfs = {0.975, 0.95, 0.90, 0.80};
        DiscountCurve dc(times, dfs);

        EXPECT_DOUBLE_EQ(dc.discount(0.5), 0.975);
        EXPECT_DOUBLE_EQ(dc.discount(1.0), 0.95);
        EXPECT_DOUBLE_EQ(dc.discount(2.0), 0.90);
        EXPECT_DOUBLE_EQ(dc.discount(5.0), 0.80);
    }

    TEST(DiscountCurve, CustomCurveInterpolation)
    {
        // Log-linear interpolation between points
        std::vector<Time> times = {1.0, 2.0};
        std::vector<Real> dfs = {0.95, 0.90};
        DiscountCurve dc(times, dfs);

        // At t=1.5, w=0.5, log_df = 0.5*ln(0.95) + 0.5*ln(0.90)
        const Real expected = std::exp(0.5 * std::log(0.95) + 0.5 * std::log(0.90));
        EXPECT_NEAR(dc.discount(1.5), expected, 1e-14);
    }

    TEST(DiscountCurve, CustomCurveFlatExtrapolationLeft)
    {
        std::vector<Time> times = {1.0, 2.0};
        std::vector<Real> dfs = {0.95, 0.90};
        DiscountCurve dc(times, dfs);

        // t < times.front() → returns dfs.front()
        EXPECT_DOUBLE_EQ(dc.discount(0.5), 0.95);
    }

    TEST(DiscountCurve, CustomCurveFlatExtrapolationRight)
    {
        std::vector<Time> times = {1.0, 2.0};
        std::vector<Real> dfs = {0.95, 0.90};
        DiscountCurve dc(times, dfs);

        // t > times.back() → returns dfs.back()
        EXPECT_DOUBLE_EQ(dc.discount(10.0), 0.90);
    }

    TEST(DiscountCurve, CustomCurveAtZero)
    {
        std::vector<Time> times = {1.0, 2.0};
        std::vector<Real> dfs = {0.95, 0.90};
        DiscountCurve dc(times, dfs);

        EXPECT_DOUBLE_EQ(dc.discount(0.0), 1.0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Validation errors
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DiscountCurve, EmptyVectorsThrow)
    {
        EXPECT_THROW(DiscountCurve({}, {}), InvalidInput);
    }

    TEST(DiscountCurve, MismatchedSizesThrow)
    {
        EXPECT_THROW(DiscountCurve({1.0, 2.0}, {0.95}), InvalidInput);
    }

    TEST(DiscountCurve, NonIncreasingTimesThrow)
    {
        EXPECT_THROW(DiscountCurve({2.0, 1.0}, {0.95, 0.90}), InvalidInput);
    }

    TEST(DiscountCurve, NegativeTimeThrows)
    {
        EXPECT_THROW(DiscountCurve({-1.0, 1.0}, {0.95, 0.90}), InvalidInput);
    }

    TEST(DiscountCurve, ZeroDFThrows)
    {
        EXPECT_THROW(DiscountCurve({1.0, 2.0}, {0.0, 0.90}), InvalidInput);
    }

    TEST(DiscountCurve, NegativeDFThrows)
    {
        EXPECT_THROW(DiscountCurve({1.0, 2.0}, {-0.05, 0.90}), InvalidInput);
    }

} // namespace quantModeling
