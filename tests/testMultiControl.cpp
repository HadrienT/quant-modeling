#include <gtest/gtest.h>

#include <Eigen/Dense>
#include <cmath>
#include <random>
#include <vector>

#include "quantModeling/utils/variance_reduction/multi_control.hpp"

// Lot G3 of blueprint/wp/19-gpu.md: the regression control-variate
// accumulator. Merging (Chan) equals adding in sequence, the estimate is the
// ordinary least-squares one, and its standard error covers the truth.

namespace quantModeling
{
    namespace
    {
        using Acc = MultiControlAccumulator;

        /// Y = 2 + 3 X1 - X2 + noise, X1 ~ N(1, 1), X2 ~ N(-0.5, 4), X3
        /// an exact copy of X1 (collinear: the solve must drop it).
        std::vector<Acc::Sample> sample(int n, unsigned seed)
        {
            std::mt19937_64 gen(seed);
            std::normal_distribution<double> z;
            std::vector<Acc::Sample> out(static_cast<std::size_t>(n));
            for (auto &s : out)
            {
                const double x1 = 1.0 + z(gen), x2 = -0.5 + 2.0 * z(gen);
                s.v[1] = x1;
                s.v[2] = x2;
                s.v[3] = x1;
                s.v[0] = 2.0 + 3.0 * x1 - x2 + 0.5 * z(gen);
            }
            return out;
        }
        const Real kMu[3] = {1.0, -0.5, 1.0};
    } // namespace

    TEST(MultiControl, MergeEqualsSequentialAdd)
    {
        const auto s = sample(1000, 3);
        Acc all, left, right;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            all.add(s[i]);
            (i < 377 ? left : right).add(s[i]);
        }
        left.merge(right);
        EXPECT_EQ(left.n, all.n);
        for (int i = 0; i < Acc::kDim; ++i)
            EXPECT_NEAR(left.mean[i], all.mean[i], 1e-12);
        for (int i = 0; i < Acc::kPacked; ++i)
            EXPECT_NEAR(left.co[i], all.co[i], 1e-9 * std::max(1.0, std::abs(all.co[i])));
    }

    TEST(MultiControl, EstimateIsOrdinaryLeastSquares)
    {
        const auto s = sample(2000, 5);
        Acc acc;
        for (const auto &x : s)
            acc.add(x);
        const Acc::Estimate e = acc.estimate(kMu, 3);
        EXPECT_EQ(e.used, 2); // X3 duplicates X1

        // OLS of Y on (1, X1 - mu1, X2 - mu2): the intercept is the estimate.
        Eigen::MatrixXd A(static_cast<Eigen::Index>(s.size()), 3);
        Eigen::VectorXd y(static_cast<Eigen::Index>(s.size()));
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const auto r = static_cast<Eigen::Index>(i);
            A(r, 0) = 1.0;
            A(r, 1) = s[i].v[1] - kMu[0];
            A(r, 2) = s[i].v[2] - kMu[1];
            y(r) = s[i].v[0];
        }
        const Eigen::VectorXd beta = A.colPivHouseholderQr().solve(y);
        EXPECT_NEAR(e.value, beta(0), 1e-10);
        EXPECT_NEAR(e.beta[0] + e.beta[2], beta(1), 1e-9);
        EXPECT_NEAR(e.beta[1], beta(2), 1e-9);

        // Standard error of the intercept: s^2 (A'A)^-1 [0, 0].
        const Eigen::VectorXd resid = y - A * beta;
        const double s2 = resid.squaredNorm() / static_cast<double>(s.size() - 3);
        const Eigen::MatrixXd cov = s2 * (A.transpose() * A).inverse();
        EXPECT_NEAR(e.std_error, std::sqrt(cov(0, 0)), 1e-10);
        EXPECT_LT(e.std_error, 0.2 * e.plain_std_error); // residual 0.25 of a variance 13.25
    }

    // Over independent runs, the estimate's spread is its reported error and
    // the truth, E[Y] = 2 + 3 (1) - (-0.5) = 5.5, is covered.
    TEST(MultiControl, StandardErrorIsCalibrated)
    {
        std::vector<double> est;
        double se_mean = 0.0;
        for (unsigned r = 0; r < 200; ++r)
        {
            Acc acc;
            for (const auto &x : sample(400, 100 + r))
                acc.add(x);
            const auto e = acc.estimate(kMu, 2);
            est.push_back(e.value);
            se_mean += e.std_error / 200.0;
        }
        double m = 0.0, v = 0.0;
        for (double x : est)
            m += x / 200.0;
        for (double x : est)
            v += (x - m) * (x - m) / 199.0;
        EXPECT_NEAR(m, 5.5, 4.0 * std::sqrt(v / 200.0));
        EXPECT_NEAR(std::sqrt(v), se_mean, 0.15 * se_mean);
    }

} // namespace quantModeling
