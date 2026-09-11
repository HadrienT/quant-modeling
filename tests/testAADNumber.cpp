#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>

namespace quantModeling::aad
{
    namespace
    {
        std::size_t count_nodes(Tape &tape)
        {
            std::size_t n = 0;
            for (Tape::iterator it = tape.begin(); it != tape.end(); ++it)
                ++n;
            return n;
        }

        /// Every operator test needs a fresh tape so node counts and
        /// pointer-address reuse in other tests do not leak in.
        struct FreshTape
        {
            FreshTape() { Number::tape->clear(); }
        };
    } // namespace

    // ── every line of the derivative table (blueprint §5.2): adjoint vs. a
    // centered finite difference, on a small grid of points ────────────────

    TEST(AADNumber, AdditionMatchesFiniteDifference)
    {
        FreshTape _;
        for (double x : {-2.0, 0.5, 3.0})
            for (double y : {-1.5, 0.1, 4.0})
            {
                Number a(x), b(y);
                Number c = a + b;
                c.propagate_to_start();
                EXPECT_NEAR(a.adjoint(), 1.0, 1e-7);
                EXPECT_NEAR(b.adjoint(), 1.0, 1e-7);
            }
    }

    TEST(AADNumber, SubtractionMatchesFiniteDifference)
    {
        FreshTape _;
        for (double x : {-2.0, 0.5, 3.0})
            for (double y : {-1.5, 0.1, 4.0})
            {
                Number a(x), b(y);
                Number c = a - b;
                c.propagate_to_start();
                EXPECT_NEAR(a.adjoint(), 1.0, 1e-7);
                EXPECT_NEAR(b.adjoint(), -1.0, 1e-7);
            }
    }

    TEST(AADNumber, MultiplicationMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {-2.0, 0.5, 3.0})
            for (double y : {-1.5, 0.1, 4.0})
            {
                Number a(x), b(y);
                Number c = a * b;
                c.propagate_to_start();
                const double fd_x = ((x + h) * y - (x - h) * y) / (2 * h);
                const double fd_y = (x * (y + h) - x * (y - h)) / (2 * h);
                EXPECT_NEAR(a.adjoint(), fd_x, 1e-7);
                EXPECT_NEAR(b.adjoint(), fd_y, 1e-7);
            }
    }

    TEST(AADNumber, DivisionMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {-2.0, 0.5, 3.0})
            for (double y : {-4.0, 0.7, 2.5}) // never zero
            {
                Number a(x), b(y);
                Number c = a / b;
                c.propagate_to_start();
                const double fd_x = ((x + h) / y - (x - h) / y) / (2 * h);
                const double fd_y = (x / (y + h) - x / (y - h)) / (2 * h);
                EXPECT_NEAR(a.adjoint(), fd_x, 1e-7);
                EXPECT_NEAR(b.adjoint(), fd_y, 1e-7);
            }
    }

    TEST(AADNumber, PowMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {0.5, 1.3, 3.0}) // positive base: pow's y-derivative needs log(x)
            for (double y : {-1.5, 0.7, 2.2})
            {
                Number a(x), b(y);
                Number c = pow(a, b);
                c.propagate_to_start();
                const double fd_x = (std::pow(x + h, y) - std::pow(x - h, y)) / (2 * h);
                const double fd_y = (std::pow(x, y + h) - std::pow(x, y - h)) / (2 * h);
                EXPECT_NEAR(a.adjoint(), fd_x, 1e-6);
                EXPECT_NEAR(b.adjoint(), fd_y, 1e-6);
            }
    }

    TEST(AADNumber, MaxMatchesFiniteDifferenceAwayFromTheKink)
    {
        FreshTape _;
        for (double x : {-2.0, 3.0})
            for (double y : {-1.0, 5.0})
            {
                Number a(x), b(y);
                Number c = max(a, b);
                c.propagate_to_start();
                if (x > y)
                {
                    EXPECT_NEAR(a.adjoint(), 1.0, 1e-12);
                    EXPECT_NEAR(b.adjoint(), 0.0, 1e-12);
                }
                else
                {
                    EXPECT_NEAR(a.adjoint(), 0.0, 1e-12);
                    EXPECT_NEAR(b.adjoint(), 1.0, 1e-12);
                }
            }
    }

    TEST(AADNumber, MinMatchesFiniteDifferenceAwayFromTheKink)
    {
        FreshTape _;
        for (double x : {-2.0, 3.0})
            for (double y : {-1.0, 5.0})
            {
                Number a(x), b(y);
                Number c = min(a, b);
                c.propagate_to_start();
                if (x < y)
                {
                    EXPECT_NEAR(a.adjoint(), 1.0, 1e-12);
                    EXPECT_NEAR(b.adjoint(), 0.0, 1e-12);
                }
                else
                {
                    EXPECT_NEAR(a.adjoint(), 0.0, 1e-12);
                    EXPECT_NEAR(b.adjoint(), 1.0, 1e-12);
                }
            }
    }

    TEST(AADNumber, UnaryMinusMatchesFiniteDifference)
    {
        FreshTape _;
        Number x(2.5);
        Number y = -x;
        y.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), -1.0, 1e-12);
    }

    TEST(AADNumber, ExpMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {-1.0, 0.0, 1.5})
        {
            Number a(x);
            Number y = exp(a);
            y.propagate_to_start();
            const double fd = (std::exp(x + h) - std::exp(x - h)) / (2 * h);
            EXPECT_NEAR(a.adjoint(), fd, 1e-6);
        }
    }

    TEST(AADNumber, LogMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {0.2, 1.0, 5.0})
        {
            Number a(x);
            Number y = log(a);
            y.propagate_to_start();
            const double fd = (std::log(x + h) - std::log(x - h)) / (2 * h);
            EXPECT_NEAR(a.adjoint(), fd, 1e-6);
        }
    }

    TEST(AADNumber, SqrtMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {0.3, 1.0, 9.0})
        {
            Number a(x);
            Number y = sqrt(a);
            y.propagate_to_start();
            const double fd = (std::sqrt(x + h) - std::sqrt(x - h)) / (2 * h);
            EXPECT_NEAR(a.adjoint(), fd, 1e-6);
        }
    }

    TEST(AADNumber, FabsMatchesFiniteDifferenceAwayFromZero)
    {
        FreshTape _;
        for (double x : {-3.0, 2.0})
        {
            Number a(x);
            Number y = fabs(a);
            y.propagate_to_start();
            EXPECT_NEAR(a.adjoint(), x >= 0.0 ? 1.0 : -1.0, 1e-12);
        }
    }

    TEST(AADNumber, NormalDensMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {-1.5, 0.0, 2.0})
        {
            Number a(x);
            Number y = normal_dens(a);
            y.propagate_to_start();
            const double fd = (norm_pdf(x + h) - norm_pdf(x - h)) / (2 * h);
            EXPECT_NEAR(a.adjoint(), fd, 1e-6);
        }
    }

    TEST(AADNumber, NormalCdfMatchesFiniteDifference)
    {
        FreshTape _;
        const double h = 1e-6;
        for (double x : {-1.5, 0.0, 2.0})
        {
            Number a(x);
            Number y = normal_cdf(a);
            y.propagate_to_start();
            const double fd = (norm_cdf(x + h) - norm_cdf(x - h)) / (2 * h);
            EXPECT_NEAR(a.adjoint(), fd, 1e-6);
        }
    }

    // ── mixed Number/double operations record a single unary node, never a
    // binary node with a wasted leaf for the constant ─────────────────────

    TEST(AADNumber, MixedMultiplicationRecordsOneNodeNotTwo)
    {
        FreshTape _;
        Number x(3.0);
        const std::size_t before = count_nodes(*Number::tape);
        Number y = x * 2.0;
        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        y.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), 2.0, 1e-12);
    }

    TEST(AADNumber, MixedAdditionBothOrdersRecordOneNode)
    {
        FreshTape _;
        Number x(3.0);
        const std::size_t before = count_nodes(*Number::tape);
        Number y = 5.0 + x;
        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        y.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), 1.0, 1e-12);
    }

    TEST(AADNumber, MaxWithZeroTheMostCommonPayoffShapeRecordsOneNode)
    {
        FreshTape _;
        Number x(-1.0);
        const std::size_t before = count_nodes(*Number::tape);
        Number payoff = max(x, 0.0);
        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        EXPECT_NEAR(payoff.value(), 0.0, 1e-12);
        payoff.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), 0.0, 1e-12); // x lost: derivative is 0, not 1
    }

    // ── toy function with variable reuse (book style): adjoint gradient
    // equals a hand-derived analytic gradient, at machine precision ────────
    //
    //   x = a*b                       (reused three times below)
    //   f = (x + c) * (x - d) + x*e
    //     = x^2 + x*(c - d + e) - c*d
    //
    // so, exactly:
    //   df/dx = 2x + (c - d + e)
    //   df/da = df/dx * b,  df/db = df/dx * a
    //   df/dc = x - d,      df/dd = -x - c,      df/de = x

    TEST(AADNumber, ToyFunctionWithVariableReuseMatchesHandDerivedGradient)
    {
        FreshTape _;
        const double av = 1.3, bv = 0.7, cv = 2.1, dv = 0.4, ev = 1.8;

        Number a(av), b(bv), c(cv), d(dv), e(ev);
        Number x = a * b;
        Number f = (x + c) * (x - d) + x * e;
        f.propagate_to_start();

        const double xv = av * bv;
        const double df_dx = 2.0 * xv + (cv - dv + ev);
        EXPECT_NEAR(a.adjoint(), df_dx * bv, 1e-10);
        EXPECT_NEAR(b.adjoint(), df_dx * av, 1e-10);
        EXPECT_NEAR(c.adjoint(), xv - dv, 1e-10);
        EXPECT_NEAR(d.adjoint(), -xv - cv, 1e-10);
        EXPECT_NEAR(e.adjoint(), xv, 1e-10);
    }

} // namespace quantModeling::aad
