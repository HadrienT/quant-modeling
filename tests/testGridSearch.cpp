// mc::lower_index_from: the grid search started from the path's last answer
// (WP 26 §25, issue #361). Its contract is lower_index's answer exactly,
// whatever the hint, so a lookup that carries hints gives the same bits.

#include <gtest/gtest.h>

#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/scripting/ir_grid.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace quantModeling
{
    namespace
    {
        bool same_bits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }

        // Sorted grids of the shapes the models see: uniform, uniform in
        // log-moneyness (the page's Dupire grid), and with repeated nodes.
        std::vector<std::vector<Real>> grids()
        {
            std::vector<std::vector<Real>> out;
            std::vector<Real> uniform, logm, repeated;
            for (int i = 0; i < 100; ++i)
                uniform.push_back(40.0 + 1.6 * i);
            for (int i = 0; i < 100; ++i)
                logm.push_back(100.0 * std::exp(-0.6 + 1.2 * i / 99.0));
            for (int i = 0; i < 20; ++i)
                repeated.push_back(static_cast<Real>(i / 3));
            out.push_back(uniform);
            out.push_back(logm);
            out.push_back(repeated);
            out.push_back({1.0, 2.0});
            out.push_back({5.0});
            return out;
        }
    } // namespace

    // Any hint, in the grid or not, and any value -- inside, outside, on a
    // node, NaN: the answer is lower_index's, and the hint becomes it.
    TEST(GridSearch, FromAnyHintIsLowerIndex)
    {
        std::mt19937_64 rng(42);
        for (const auto &x : grids())
        {
            const int n = static_cast<int>(x.size());
            std::vector<double> values{std::numeric_limits<double>::quiet_NaN(),
                                       -std::numeric_limits<double>::infinity(),
                                       std::numeric_limits<double>::infinity(), x.front() - 1.0, x.back() + 1.0};
            for (const Real k : x)
                values.push_back(k), values.push_back(std::nextafter(k, -1e300)),
                    values.push_back(std::nextafter(k, 1e300));
            std::uniform_real_distribution<double> u(x.front() - 5.0, x.back() + 5.0);
            for (int i = 0; i < 200; ++i)
                values.push_back(u(rng));
            for (const double v : values)
                for (int hint0 = -3; hint0 <= n + 3; ++hint0)
                {
                    int hint = hint0;
                    const int want = mc::lower_index(x.data(), n, v);
                    ASSERT_EQ(mc::lower_index_from(x.data(), n, v, hint), want) << "v " << v << " hint " << hint0;
                    ASSERT_EQ(hint, want);
                }
        }
    }

    // Along random walks, near and far, the hint carried from step to step
    // gives the bracket the plain search gives.
    TEST(GridSearch, BracketAlongAWalkIsTheSame)
    {
        std::mt19937_64 rng(7);
        for (const auto &x : grids())
        {
            if (x.size() < 2)
                continue;
            const int n = static_cast<int>(x.size());
            for (const double step : {0.01, 1.0, 5.0, 40.0})
            {
                std::normal_distribution<double> z(0.0, step);
                double v = 0.5 * (x.front() + x.back());
                int hint = -1;
                for (int s = 0; s < 2000; ++s)
                {
                    v += z(rng);
                    int a0, a1, b0, b1;
                    mc::bracket(x.data(), n, v, a0, a1);
                    mc::bracket(x.data(), n, v, b0, b1, &hint);
                    ASSERT_EQ(a0, b0);
                    ASSERT_EQ(a1, b1);
                }
            }
        }
    }

    // The lookups the GPU's steps make, with and without hints, along a
    // path: the same bits (local vol, SLV's leverage, and their cells).
    TEST(GridSearch, HintedLookupsKeepTheirBits)
    {
        std::vector<Real> K, T, values;
        for (int i = 0; i < 100; ++i)
            K.push_back(100.0 * std::exp(-0.6 + 1.2 * i / 99.0));
        for (int j = 0; j < 129; ++j)
            T.push_back(2.0 * (j + 1) / 129.0);
        for (int i = 0; i < 100; ++i)
            for (int j = 0; j < 129; ++j)
                values.push_back(0.15 + 0.1 * std::cos(0.07 * i + 0.03 * j) + 0.001 * j);
        const mc::GridView<Real> g{K.data(), 100, T.data(), 129, values.data()};

        std::mt19937_64 rng(3);
        std::normal_distribution<double> z(0.0, 1.0);
        mc::GridHint fwd, cell;
        double S = 100.0;
        for (int s = 0; s < 504; ++s)
        {
            const double t = (s + 1) / 252.0;
            S *= std::exp(0.25 * std::sqrt(1.0 / 252.0) * z(rng));
            ASSERT_TRUE(same_bits(mc::local_vol_bilinear<Real>(g, S, t), mc::local_vol_bilinear<Real>(g, S, t, &fwd)));
            ASSERT_TRUE(same_bits(mc::leverage_staircase<Real>(g, S, t), mc::leverage_staircase<Real>(g, S, t, &fwd)));

            namespace grid = scripting::ir::detail;
            const grid::GridCell a = grid::grid_cell(g, S, t), b = grid::grid_cell(g, S, t, &cell);
            ASSERT_TRUE(same_bits(a.sigma, b.sigma));
            ASSERT_EQ(a.j0, b.j0);
            for (int k = 0; k < 4; ++k)
            {
                ASSERT_EQ(a.idx[k], b.idx[k]);
                ASSERT_TRUE(same_bits(a.w[k], b.w[k]));
            }
            const grid::LeverageCell la = grid::leverage_cell(g, S, t), lb = grid::leverage_cell(g, S, t, &cell);
            ASSERT_EQ(la.i0, lb.i0);
            ASSERT_EQ(la.j, lb.j);
            ASSERT_TRUE(same_bits(la.wK, lb.wK));
        }
    }
} // namespace quantModeling
