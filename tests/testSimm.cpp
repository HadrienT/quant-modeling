#include <gtest/gtest.h>

#include "quantModeling/risk/simm.hpp"

#include <cmath>
#include <limits>

// Lot X5b of blueprint/wp/23-xva.md: ISDA SIMM for the interest rate risk
// class of one currency. Parameters of version 2.8+2512, checked here against
// the values printed in the public document.

namespace quantModeling
{
    using namespace simm;

    TEST(Simm, ParametersAreThoseOfVersion2_8_2512)
    {
        // Table 1 (§33) and the thresholds of §74 and §81.
        const CurrencyParameters usd = regular_well_traded();
        const std::array<Real, vertices> table1 = {107, 101, 90, 69, 68, 69, 66, 61, 60, 58, 58, 66};
        EXPECT_EQ(usd.risk_weight, table1);
        EXPECT_EQ(usd.delta_concentration_threshold, 220e6);
        EXPECT_EQ(usd.vega_concentration_threshold, 3800e6);
        EXPECT_EQ(regular_less_traded().risk_weight, table1);
        EXPECT_EQ(regular_less_traded().delta_concentration_threshold, 110e6);
        EXPECT_EQ(low_volatility().risk_weight[8], 29.0);   // JPY, 10y (Table 2)
        EXPECT_EQ(high_volatility().risk_weight[0], 167.0); // 2w (Table 3)
        EXPECT_EQ(vega_risk_weight, 0.20);                  // §35
        EXPECT_EQ(historical_volatility_ratio, 0.74);       // §34

        // The correlation matrix of §36: symmetric, ones on the diagonal,
        // falling with the distance between vertices from 2y on.
        for (std::size_t k = 0; k < vertices; ++k)
        {
            EXPECT_EQ(correlation[k][k], 1.0);
            for (std::size_t l = 0; l < vertices; ++l)
                EXPECT_EQ(correlation[k][l], correlation[l][k]);
        }
        EXPECT_EQ(correlation[0][1], 0.74);  // 2w, 1m
        EXPECT_EQ(correlation[4][8], 0.73);  // 1y, 10y
        EXPECT_EQ(correlation[8][11], 0.95); // 10y, 30y
        EXPECT_EQ(correlation[9][11], 0.97); // 15y, 30y

        // The scaling function, against the example table of §11a.
        EXPECT_NEAR(scaling_function(14.0 / 365.0), 0.500, 1e-12);
        EXPECT_NEAR(scaling_function(1.0 / 12.0), 0.230, 5e-4);
        EXPECT_NEAR(scaling_function(0.25), 0.077, 5e-4);
        EXPECT_NEAR(scaling_function(1.0), 0.019, 5e-4);
        EXPECT_NEAR(scaling_function(5.0), 0.004, 5e-4);
        EXPECT_NEAR(scaling_function(10.0), 0.002, 5e-4);
    }

    TEST(Simm, DeltaMarginOfOneVertexIsItsRiskWeightTimesItsPv01)
    {
        Sensitivities s;
        s.delta[8] = -7800.0; // a 10-year payer swap of 10 M: about 7 800 a basis point
        const Margin m = interest_rate_margin(s);
        // 60 bp of move on 7 800 a bp, whatever the sign.
        EXPECT_NEAR(m.delta, 60.0 * 7800.0, 1e-9);
        EXPECT_EQ(m.vega, 0.0);
        EXPECT_EQ(m.curvature, 0.0);
        EXPECT_EQ(m.total(), m.delta);
        s.delta[8] = 7800.0;
        EXPECT_NEAR(interest_rate_margin(s).delta, 60.0 * 7800.0, 1e-9);
        // Homogeneous below the concentration threshold: twice the position,
        // twice the margin.
        s.delta[8] = 15600.0;
        EXPECT_NEAR(interest_rate_margin(s).delta, 2.0 * 60.0 * 7800.0, 1e-9);
    }

    TEST(Simm, TwoVerticesAggregateWithTheirCorrelation)
    {
        Sensitivities s;
        s.delta[7] = 1000.0; // 5y
        s.delta[8] = 2000.0; // 10y
        const Real a = 61.0 * 1000.0, b = 60.0 * 2000.0, rho = 0.96;
        EXPECT_NEAR(interest_rate_margin(s).delta, std::sqrt(a * a + b * b + 2.0 * rho * a * b), 1e-9);
        // A steepener: long one vertex, short the other. Less margin than
        // either leg added, more than nothing — the vertices are not the
        // same risk.
        s.delta[7] = -1000.0;
        const Real hedged = interest_rate_margin(s).delta;
        EXPECT_NEAR(hedged, std::sqrt(a * a + b * b - 2.0 * rho * a * b), 1e-9);
        EXPECT_LT(hedged, b);
        EXPECT_GT(hedged, b - a);
    }

    TEST(Simm, AConcentratedPositionPaysMoreThanItsShare)
    {
        // Net PV01 four times the threshold of 220 M per bp: CR = 2.
        Sensitivities s;
        s.delta[8] = 4.0 * 220e6;
        EXPECT_NEAR(interest_rate_margin(s).delta, 2.0 * 60.0 * 4.0 * 220e6, 1.0);
        // The same PV01 in a less traded currency: a lower threshold.
        EXPECT_GT(interest_rate_margin(s, regular_less_traded()).delta,
                  interest_rate_margin(s).delta);
        // Offsetting vertices net out of the concentration measure.
        s.delta[7] = -4.0 * 220e6;
        Sensitivities small = s;
        small.delta[7] *= 1e-6;
        small.delta[8] *= 1e-6;
        EXPECT_NEAR(interest_rate_margin(s).delta, 1e6 * interest_rate_margin(small).delta, 1.0);
    }

    TEST(Simm, ACashFlowBetweenTwoVerticesIsSharedBetweenThem)
    {
        Sensitivities s;
        s.add_delta(7.0, 100.0); // between 5y and 10y: 60 % / 40 %
        EXPECT_NEAR(s.delta[7], 60.0, 1e-12);
        EXPECT_NEAR(s.delta[8], 40.0, 1e-12);
        s = {};
        s.add_delta(10.0, 100.0);
        EXPECT_NEAR(s.delta[8], 100.0, 1e-12);
        s = {};
        s.add_delta(45.0, 100.0); // beyond the last vertex
        EXPECT_EQ(s.delta[11], 100.0);
        s = {};
        s.add_delta(0.01, 100.0); // before the first
        EXPECT_EQ(s.delta[0], 100.0);
        // Whatever the time, nothing is lost.
        s = {};
        for (const Time t : {0.02, 0.3, 1.7, 4.0, 12.0, 26.0, 33.0})
            s.add_delta(t, 1.0);
        Real total = 0.0;
        for (const Real d : s.delta)
            total += d;
        EXPECT_NEAR(total, 7.0, 1e-12);
    }

    TEST(Simm, VegaAndCurvatureOfABoughtOption)
    {
        // A swaption expiring in one year, vega × vol of 50 000.
        Sensitivities s;
        s.add_vega(1.0, 50000.0);
        const Margin m = interest_rate_margin(s);
        EXPECT_NEAR(m.vega, 0.20 * 50000.0, 1e-9);
        // Curvature of a long option (CVR > 0): theta = 0, lambda =
        // Φ⁻¹(0.995)² - 1, and one vertex: (1 + lambda) CVR / HVR².
        const Real cvr = scaling_function(1.0) * 50000.0;
        const Real q = 2.5758293035489;
        EXPECT_NEAR(m.curvature, q * q * cvr / (0.74 * 0.74), 1e-6 * m.curvature);
        EXPECT_GT(m.curvature, 0.0);
        EXPECT_NEAR(m.total(), m.vega + m.curvature, 1e-12);

        // Sold: the same vega margin, and a curvature that is not charged —
        // theta = -1 makes lambda 1, and CVR + K = 0.
        Sensitivities sold;
        sold.add_vega(1.0, -50000.0);
        const Margin short_option = interest_rate_margin(sold);
        EXPECT_NEAR(short_option.vega, m.vega, 1e-9);
        EXPECT_NEAR(short_option.curvature, 0.0, 1e-9);

        // A short-dated option has more gamma for the same vega.
        Sensitivities near;
        near.add_vega(1.0 / 12.0, 50000.0);
        EXPECT_GT(interest_rate_margin(near).curvature, 5.0 * m.curvature);
    }

    TEST(Simm, RejectsASensitivityThatIsNotANumber)
    {
        Sensitivities s;
        s.delta[3] = std::numeric_limits<Real>::quiet_NaN();
        EXPECT_THROW(interest_rate_margin(s), InvalidInput);
        s = {};
        s.vega[3] = std::numeric_limits<Real>::infinity();
        EXPECT_THROW(interest_rate_margin(s), InvalidInput);
        // Nothing to margin: nothing.
        EXPECT_EQ(interest_rate_margin({}).total(), 0.0);
    }

} // namespace quantModeling
