#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>

// mc::qe_step, the QE-M step shared by the price runs of Heston and SLV on
// the CPU and the GPU (#317). Property tests against COS: the price within
// its error at a coarse grid where full-truncation Euler is far off, the
// forward kept (the martingale correction), V >= 0, and the leverage L
// entering as a scale of the spot's vol.

namespace quantModeling
{
    namespace
    {
        constexpr double kS0 = 100.0, kR = 0.04, kQ = 0.012, kT = 1.0;
        constexpr std::uint64_t kSeed = 317;
        // SPY's calibrated fit of 2026-11-18: 2 kappa theta / xi^2 = 0.13
        constexpr mc::HestonParamsT<double> kSpy{0.0485, 3.314, 0.0374, 1.377, -0.70};

        struct Mc
        {
            double call = 0, call_se = 0, spot = 0, spot_se = 0, min_v = 1e300;
        };

        Mc simulate(const mc::HestonParamsT<double> &h, double L, int n_steps, mc::VarianceScheme scheme,
                    double strike, int n_paths = 1 << 17)
        {
            Mc out;
            double s = 0, s2 = 0, f = 0, f2 = 0;
            const double dt = kT / n_steps, sqdt = std::sqrt(dt), disc = std::exp(-kR * kT);
            for (int p = 0; p < n_paths; ++p)
            {
                double S = kS0, v = h.v0;
                for (int k = 0; k < n_steps; ++k)
                {
                    const double z0 = inverse_normal_cdf(philox_uniform(kSeed, p, 2 * k));
                    const double z1 = inverse_normal_cdf(philox_uniform(kSeed, p, 2 * k + 1));
                    if (scheme == mc::VarianceScheme::QE)
                        mc::qe_step(h, L, kR, kQ, S, v, dt, z0, z1);
                    else
                    {
                        const double vp = std::max(v, mc::variance_floor()), sv = std::sqrt(vp);
                        S = S * std::exp(mc::heston_log_return(kR, kQ, 0.0, 0.0, vp, sv, sqdt, dt, z0));
                        mc::heston_variance_step(h, v, vp, sv, sqdt, dt, z0, z1);
                    }
                    out.min_v = std::min(out.min_v, v);
                }
                const double x = disc * std::max(S - strike, 0.0);
                s += x, s2 += x * x, f += S, f2 += S * S;
            }
            out.call = s / n_paths;
            out.call_se = std::sqrt((s2 / n_paths - out.call * out.call) / n_paths);
            out.spot = f / n_paths;
            out.spot_se = std::sqrt((f2 / n_paths - out.spot * out.spot) / n_paths);
            return out;
        }

        double cos_call(const mc::HestonParamsT<double> &h, double strike)
        {
            return heston_cos_price(kS0 * std::exp((kR - kQ) * kT), strike, kT, std::exp(-kR * kT),
                                    HestonParams{h.v0, h.kappa, h.theta, h.xi, h.rho}, true);
        }
    } // namespace

    TEST(HestonQEStep, AgreesWithCOSAtSixteenStepsAYearWhereEulerIsFarOff)
    {
        // Measured (benchmarks/heston_qe_bias.cpp, 4M paths): at 16 steps a
        // year Euler is +6.7 % on the ATM call, QE +0.1 %.
        const double exact = cos_call(kSpy, 100.0);
        const Mc qe = simulate(kSpy, 1.0, 16, mc::VarianceScheme::QE, 100.0);
        const Mc euler = simulate(kSpy, 1.0, 16, mc::VarianceScheme::Euler, 100.0);
        EXPECT_NEAR(qe.call, exact, 4.0 * qe.call_se + 0.003 * exact);
        EXPECT_GT(euler.call - exact, 0.03 * exact);
    }

    TEST(HestonQEStep, KeepsTheForwardAndANonNegativeVariance)
    {
        for (int n : {2, 8, 64})
        {
            const Mc qe = simulate(kSpy, 1.0, n, mc::VarianceScheme::QE, 100.0);
            EXPECT_NEAR(qe.spot, kS0 * std::exp((kR - kQ) * kT), 4.0 * qe.spot_se) << n << " steps";
            EXPECT_GE(qe.min_v, 0.0);
        }
    }

    TEST(HestonQEStep, ALeverageKeepsTheForward)
    {
        // SLV's step: the correction taken with L at the step's start. With a
        // constant L the forward still holds exactly in expectation.
        for (double L : {0.5, 1.7})
        {
            const Mc qe = simulate(kSpy, L, 32, mc::VarianceScheme::QE, 100.0);
            EXPECT_NEAR(qe.spot, kS0 * std::exp((kR - kQ) * kT), 4.0 * qe.spot_se) << "L " << L;
        }
    }

    TEST(HestonQEStep, ConstantLeverageIsHestonWithAScaledVariance)
    {
        // L sqrt(V) with V Heston(v0, kappa, theta, xi) is sqrt(W) with W =
        // L^2 V Heston(L^2 v0, kappa, L^2 theta, L xi): the same price.
        const double L = 1.3;
        mc::HestonParamsT<double> scaled = kSpy;
        scaled.v0 *= L * L, scaled.theta *= L * L, scaled.xi *= L;
        const Mc qe = simulate(kSpy, L, 32, mc::VarianceScheme::QE, 100.0);
        const double exact = cos_call(scaled, 100.0);
        EXPECT_NEAR(qe.call, exact, 4.0 * qe.call_se + 0.003 * exact);
    }
} // namespace quantModeling
