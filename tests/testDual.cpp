#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/utils/dual.hpp"

// Lot G3 of blueprint/wp/19-gpu.md: forward-mode duals give the CPU tape's
// derivatives -- same local derivatives, same side taken at a kink.

namespace quantModeling
{
    namespace
    {
        /// A function exercising every operation the script interpreter and
        /// the models use, written once for any number type.
        template <class T>
        T mix(const T &x, const T &y)
        {
            using std::exp;
            using std::fabs;
            using std::log;
            using std::max;
            using std::min;
            using std::pow;
            using std::sqrt;
            T a = exp(x * y) / sqrt(x) + log(y) - pow(x, y);
            a = a + max(x, y) * fabs(x - y) - min(x, 2.0 * y) / (1.0 + x);
            a = a * (3.0 - y) + max(a, 0.5) - 2.0 / y;
            T b = -a;
            b += x;
            b -= y * 0.25;
            b *= x;
            b /= (y + 1.0);
            return b + max(0.1, x) + min(y, 0.7);
        }

        struct FreshTape
        {
            FreshTape() { aad::Number::tape->clear(); }
        };
    } // namespace

    TEST(Dual, EveryOperationMatchesTheTape)
    {
        FreshTape _;
        for (double x : {0.3, 0.9, 1.7})
            for (double y : {0.4, 1.1, 2.5})
            {
                const Dual<2> dx = Dual<2>::variable(x, 0), dy = Dual<2>::variable(y, 1);
                const Dual<2> f = mix(dx, dy);

                aad::Number::tape->rewind();
                aad::Number ax(x), ay(y);
                aad::Number g = mix(ax, ay);
                g.propagate_to_start();

                EXPECT_NEAR(f.value(), g.value(), 1e-13 * std::max(1.0, std::abs(g.value()))) << x << " " << y;
                EXPECT_NEAR(f.d[0], ax.adjoint(), 1e-12 * std::max(1.0, std::abs(ax.adjoint()))) << x << " " << y;
                EXPECT_NEAR(f.d[1], ay.adjoint(), 1e-12 * std::max(1.0, std::abs(ay.adjoint()))) << x << " " << y;
            }
    }

    // At a tie the tape differentiates max's right argument and min's right
    // argument; duals must pick the same side.
    TEST(Dual, KinksTakeTheTapesSide)
    {
        const Dual<2> x = Dual<2>::variable(1.0, 0), y = Dual<2>::variable(1.0, 1);
        EXPECT_EQ(max(x, y).d[1], 1.0);
        EXPECT_EQ(max(x, y).d[0], 0.0);
        EXPECT_EQ(min(x, y).d[1], 1.0);
        EXPECT_EQ(fabs(Dual<2>::variable(0.0, 0)).d[0], 1.0);
        // Comparisons read the value only.
        EXPECT_TRUE(x == y);
        EXPECT_FALSE(x < 1.0);
    }

} // namespace quantModeling
