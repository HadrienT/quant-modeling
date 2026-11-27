#include <gtest/gtest.h>

#include "quantModeling/market/forward_curve.hpp"

#include <cmath>
#include <cstddef>
#include <random>
#include <utility>
#include <vector>

// WP 32 F1: the forward curve every equity model will read
// (blueprint/wp/32-forward-curve.md).

namespace quantModeling
{
    namespace
    {
        // An SPX-like term structure (WP 31 §4.7): the implied rate rising
        // from 4.4 % at two months to 5.1 % at two years, a carry below it.
        ForwardCurve market_like_curve()
        {
            const double S0 = 6000.0;
            const std::vector<Time> T{2.0 / 12, 0.5, 1.0, 2.0};
            const std::vector<Real> r{0.044, 0.047, 0.050, 0.051};
            const std::vector<Real> q{0.012, 0.013, 0.0135, 0.014};
            std::vector<Real> F, DF;
            for (std::size_t i = 0; i < T.size(); ++i)
            {
                F.push_back(S0 * std::exp((r[i] - q[i]) * T[i]));
                DF.push_back(std::exp(-r[i] * T[i]));
            }
            return ForwardCurve::from_pillars(S0, T, F, DF);
        }

        const std::vector<Time> kQueryTimes{0.0, 1e-3, 0.05, 2.0 / 12, 0.3, 0.5, 0.77, 1.0, 1.5, 2.0, 3.0, 7.5};
    } // namespace

    // The flat curve evaluates exactly the scalar expressions the models use
    // today: the condition for today's prices to stay bit for bit (WP 32 F1).
    TEST(ForwardCurve, FlatCurveIsBitForBitTheScalarExpressions)
    {
        const double S0 = 100.0, r = 0.043, q = 0.0125;
        const ForwardCurve c = ForwardCurve::flat(S0, r, q);
        ASSERT_TRUE(c.is_flat());
        EXPECT_EQ(c.flat_rate(), r);
        EXPECT_EQ(c.flat_dividend(), q);
        for (const Time t : kQueryTimes)
        {
            EXPECT_EQ(c.forward(t), S0 * std::exp((r - q) * t)) << t;
            EXPECT_EQ(c.discount(t), std::exp(-r * t)) << t;
            EXPECT_EQ(c.log_forward(t), (r - q) * t) << t;
            for (const Time u : kQueryTimes)
            {
                EXPECT_EQ(c.carry(t, u), (r - q) * (u - t));
                EXPECT_EQ(c.rate(t, u), r * (u - t));
                EXPECT_EQ(c.dividend(t, u), q * (u - t));
            }
        }
    }

    // A curve built on flat pillars agrees with the flat curve (the general
    // path, to rounding).
    TEST(ForwardCurve, FlatPillarsAgreeWithTheFlatCurve)
    {
        const double S0 = 100.0, r = 0.043, q = 0.0125;
        const std::vector<Time> T{0.25, 1.0, 3.0};
        std::vector<Real> F, DF;
        for (const Time t : T)
        {
            F.push_back(S0 * std::exp((r - q) * t));
            DF.push_back(std::exp(-r * t));
        }
        const ForwardCurve c = ForwardCurve::from_pillars(S0, T, F, DF);
        const ForwardCurve flat = ForwardCurve::flat(S0, r, q);
        EXPECT_FALSE(c.is_flat());
        for (const Time t : kQueryTimes)
        {
            EXPECT_NEAR(c.forward(t) / flat.forward(t), 1.0, 1e-14) << t;
            EXPECT_NEAR(c.discount(t) / flat.discount(t), 1.0, 1e-14) << t;
            EXPECT_NEAR(c.dividend(0.0, t), flat.dividend(0.0, t), 1e-14) << t;
        }
    }

    TEST(ForwardCurve, RepricesItsPillars)
    {
        const double S0 = 50.0;
        const std::vector<Time> T{0.1, 0.4, 1.2};
        const std::vector<Real> F{50.2, 50.1, 51.7}; // a dividend between 0.1 and 0.4
        const std::vector<Real> DF{0.9955, 0.982, 0.941};
        const ForwardCurve c = ForwardCurve::from_pillars(S0, T, F, DF);
        EXPECT_EQ(c.spot(), S0);
        EXPECT_EQ(c.forward(0.0), S0);
        EXPECT_EQ(c.discount(0.0), 1.0);
        for (std::size_t i = 0; i < T.size(); ++i)
        {
            EXPECT_NEAR(c.forward(T[i]) / F[i], 1.0, 1e-15) << i;
            EXPECT_NEAR(c.discount(T[i]) / DF[i], 1.0, 1e-15) << i;
        }
    }

    // Carry and rate compose over consecutive intervals: what lets a model
    // step on carry(t_i, t_i+1) and land on F(t) whatever its time grid.
    TEST(ForwardCurve, CarryAndRateTelescope)
    {
        const ForwardCurve c = market_like_curve();
        for (std::size_t n : {1, 3, 17, 250})
        {
            const Time T = 2.7; // past the last pillar: extrapolation included
            double carry = 0.0, rate = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Time a = T * double(i) / double(n), b = T * double(i + 1) / double(n);
                carry += c.carry(a, b);
                rate += c.rate(a, b);
            }
            EXPECT_NEAR(std::exp(carry) * c.spot(), c.forward(T), 1e-10 * c.forward(T)) << n;
            EXPECT_NEAR(std::exp(-rate), c.discount(T), 1e-14) << n;
        }
    }

    // Zero coupons: a payment of 1 at t discounted step by step on the
    // curve's short rate reprices DF(t), with the dividend leg the gap.
    TEST(ForwardCurve, ZeroCouponsRepriceTheDiscountCurve)
    {
        const ForwardCurve c = market_like_curve();
        for (const Time t : kQueryTimes)
        {
            EXPECT_NEAR(std::exp(-c.rate(0.0, t)), c.discount(t), 1e-15) << t;
            // F(t) DF(t) = S0 e^{−∫q}: the dividend leg is the rest.
            EXPECT_NEAR(c.forward(t) * c.discount(t), c.spot() * std::exp(-c.dividend(0.0, t)),
                        1e-12 * c.spot())
                << t;
        }
    }

    // E[S_t] = F(t): exact lognormal steps on carry(t_i, t_i+1) are a
    // martingale after the carry, on a grid that does not see the pillars.
    TEST(ForwardCurve, SimulatedSpotHasTheCurvesForward)
    {
        const ForwardCurve c = market_like_curve();
        const double sigma = 0.2;
        const std::size_t steps = 30, paths = 200000;
        const Time T = 2.4, dt = T / steps;
        std::mt19937_64 rng(20261127);
        std::normal_distribution<double> z;
        std::vector<double> sum(steps, 0.0), sum2(steps, 0.0);
        for (std::size_t p = 0; p < paths; ++p)
        {
            double S = c.spot();
            for (std::size_t i = 0; i < steps; ++i)
            {
                S *= std::exp(c.carry(double(i) * dt, double(i + 1) * dt) - 0.5 * sigma * sigma * dt +
                              sigma * std::sqrt(dt) * z(rng));
                sum[i] += S;
                sum2[i] += S * S;
            }
        }
        for (std::size_t i = 0; i < steps; ++i)
        {
            const double mean = sum[i] / double(paths);
            const double se = std::sqrt((sum2[i] / double(paths) - mean * mean) / double(paths));
            EXPECT_NEAR(mean, c.forward(double(i + 1) * dt), 4.0 * se) << i;
        }
    }

    // A discrete cash dividend as two pillars a second apart: with a flat
    // rate, F(t) = S0 e^{rt} before the ex-date and (S0 − D e^{−rτ}) e^{rt}
    // after it, both linear in ln F, so the curve is exact away from the gap.
    TEST(ForwardCurve, CashDividendIsAJumpBetweenTwoPillars)
    {
        const double S0 = 100.0, r = 0.04, D = 1.9, tau = 0.45, gap = 1.0 / (365.0 * 86400.0);
        auto exact = [&](Time t)
        { return t <= tau ? S0 * std::exp(r * t) : (S0 - D * std::exp(-r * tau)) * std::exp(r * t); };
        const std::vector<Time> T{0.2, tau, tau + gap, 1.0};
        std::vector<Real> F, DF;
        for (const Time t : T)
        {
            F.push_back(exact(t));
            DF.push_back(std::exp(-r * t));
        }
        const ForwardCurve c = ForwardCurve::from_pillars(S0, T, F, DF);
        for (const Time t : {0.05, 0.2, 0.3, tau, tau + 2 * gap, 0.6, 1.0})
            EXPECT_NEAR(c.forward(t) / exact(t), 1.0, 1e-13) << t;
        EXPECT_NEAR(c.forward(tau) - c.forward(tau + gap), D, 1e-6);
        // The jump is in the dividend leg, not in the rate.
        EXPECT_NEAR(c.rate(tau, tau + gap), r * gap, 1e-15);
        EXPECT_NEAR(c.dividend(tau, tau + gap), -std::log(1.0 - D / c.forward(tau)), 1e-9);
    }

    // log_forward_weights is the derivative of log_forward in the pillars'
    // log forwards (the risks per forward pillar of F3): it reproduces
    // log_forward, and a bump of one pillar moves log_forward by its weight.
    TEST(ForwardCurve, LogForwardWeightsAreThePillarSensitivities)
    {
        const ForwardCurve c = market_like_curve();
        const std::vector<Time> &T = c.pillar_times();
        const std::vector<Real> &y = c.pillar_log_forwards();
        std::vector<std::pair<std::size_t, Real>> w;
        const double h = 1e-6;
        for (const Time t : kQueryTimes)
        {
            c.log_forward_weights(t, w);
            double sum = 0.0;
            for (const auto &[i, wi] : w)
                sum += wi * y[i];
            EXPECT_NEAR(sum, c.log_forward(t), 1e-15) << t;
            for (std::size_t k = 0; k < T.size(); ++k)
            {
                std::vector<Real> F;
                for (std::size_t i = 0; i < T.size(); ++i)
                    F.push_back(c.spot() * std::exp(y[i] + (i == k ? h : 0.0)));
                const ForwardCurve bumped(c.spot(), c.discount_curve(), T, F);
                double expected = 0.0;
                for (const auto &[i, wi] : w)
                    if (i == k)
                        expected += wi;
                EXPECT_NEAR((bumped.log_forward(t) - c.log_forward(t)) / h, expected, 1e-8) << t << " " << k;
            }
        }
        ForwardCurve::flat(100.0, 0.04, 0.01).log_forward_weights(1.0, w);
        EXPECT_TRUE(w.empty());
    }

    TEST(ForwardCurve, RejectsInvalidInput)
    {
        const DiscountCurve dc(0.04);
        EXPECT_THROW(ForwardCurve::flat(0.0, 0.04, 0.01), InvalidInput);
        EXPECT_THROW(ForwardCurve::flat(100.0, std::nan(""), 0.01), InvalidInput);
        EXPECT_THROW(ForwardCurve(100.0, dc, {}, {}), InvalidInput);
        EXPECT_THROW(ForwardCurve(100.0, dc, {1.0}, {101.0, 102.0}), InvalidInput);
        EXPECT_THROW(ForwardCurve(100.0, dc, {0.0, 1.0}, {100.0, 101.0}), InvalidInput);
        EXPECT_THROW(ForwardCurve(100.0, dc, {1.0, 1.0}, {100.0, 101.0}), InvalidInput);
        EXPECT_THROW(ForwardCurve(100.0, dc, {1.0, 2.0}, {100.0, -1.0}), InvalidInput);
        EXPECT_THROW(ForwardCurve::from_pillars(100.0, {1.0}, {101.0}, {0.0}), InvalidInput);
    }

} // namespace quantModeling
