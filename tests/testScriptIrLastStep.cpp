#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/stats.hpp"

#include "ir_fixtures.hpp"

// WP 27 §14.9: the last-step expectations (lastStepCall / lastStepPut /
// lastStepDigital) in the IR's paths. The step leaves each asset's forward
// and log-variance; an event computes Black's formula from them. Checked on
// the host (the CI has no GPU):
//   - the IR's engine on the CPU gives the tape's price and risks, which the
//     CPU models compute their own way (Black-Scholes, local vol, Heston,
//     SLV);
//   - second order by tangent over adjoint: each Hessian row is the central
//     difference of the gradient, path by path, and under Black-Scholes the
//     gamma, vanna and volga are the closed forms.

namespace quantModeling
{
    namespace
    {
        using namespace ir_test;
        namespace ir = scripting::ir;

        constexpr std::uint64_t kSeed = 23;

        // kCtx is 2026-06-01: 2027-06-01 is one year (ACT/365F).
        const std::string kMid = "2026-12-01\n    a = spot()\n";
        const std::vector<std::string> kScripts = {
            kMid + "2027-06-01\n    pays lastStepCall(100)\n",
            kMid + "2027-06-01\n    pays lastStepPut(95) + 0.5 * lastStepDigital(105) * a / 100\n",
            // two strikes on the same slot of two events: two blocks
            "2026-12-01\n    pays lastStepCall(100)\n2027-06-01\n    pays lastStepCall(110)\n",
        };

        void expect_rows_near(const std::vector<double> &a, const std::vector<double> &b, double rel,
                              const std::string &what)
        {
            ASSERT_EQ(a.size(), b.size()) << what;
            double scale = 1e-12;
            for (double x : b)
                scale = std::max(scale, std::abs(x));
            for (std::size_t i = 0; i < a.size(); ++i)
                ASSERT_NEAR(a[i], b[i], rel * scale) << what << ", risk " << i;
        }
    } // namespace

    TEST(ScriptIrLastStep, TwoStrikesOnTheSameSlotAreTwoBlocks)
    {
        const ScriptedProduct<Real> product(kScripts[2], kCtx);
        EXPECT_TRUE(product.ir().has_last_step);
        EXPECT_FALSE(product.ir().has_bridge);
        EXPECT_EQ(product.ir().blocks.size(), 2u);
    }

    TEST(ScriptIrLastStep, TheIrsEngineOnTheCpuGivesTheTapesResults)
    {
        for (const Dyn dyn : {Dyn::BlackScholes, Dyn::LocalVol, Dyn::Heston, Dyn::SLV})
            for (const std::string &source : kScripts)
            {
                const ScriptedProduct<Real> product(source, kCtx);
                auto model = make_model<Real>(dyn, 1);
                std::string why;
                const auto got = simulate_script_aad_ir(product, *model, 200, kSeed, why);
                ASSERT_TRUE(got.has_value()) << name(dyn) << ": " << why;
                const ScriptedProduct<aad::Number> tape_product(source, kCtx);
                auto tape_model = make_model<aad::Number>(dyn, 1);
                const AADSimulResults tape =
                    simulate_aad(tape_product, *tape_model, 200, kSeed, first_aad_payoff, RngKind::Philox);
                const std::string what = std::string(name(dyn)) + "\n" + source;
                EXPECT_EQ(got->risk_labels, tape.risk_labels) << what;
                EXPECT_NEAR(got->price, tape.price, 1e-12 * std::max(1.0, std::abs(tape.price))) << what;
                expect_rows_near(got->risks, tape.risks, 1e-10, what);
            }
    }

    // Path by path, no noise: every entry of the Hessian against the central
    // difference of the gradient, and its symmetry. (Local vol and SLV refuse
    // second order through their grid, WP 27 §14.5.)
    TEST(ScriptIrLastStep, TheHessianIsTheGradientsDerivative)
    {
        for (const Dyn dyn : {Dyn::BlackScholes, Dyn::Heston})
            for (const std::string &source : kScripts)
            {
                const SecondOrderCase sc(source, dyn, {}, kSeed);
                const std::vector<double> theta = sc.theta();
                const auto K = static_cast<std::size_t>(sc.K);
                const double step = dyn == Dyn::Heston ? 1e-7 : 1e-5; // as testScriptIrAd
                for (int path = 0; path < 4; ++path)
                {
                    const SecondOrder base = sc.at(theta, path);
                    double scale = 1e-9;
                    for (std::size_t i = 0; i < K * K; ++i)
                        scale = std::max(scale, std::abs(base.hessian[i]));
                    for (std::size_t d = 0; d < K; ++d)
                    {
                        const double h = step * std::max(1.0, std::abs(theta[d]));
                        std::vector<double> up = theta, down = theta;
                        up[d] += h;
                        down[d] -= h;
                        const SecondOrder a = sc.at(up, path), b = sc.at(down, path);
                        for (std::size_t p = 0; p < K; ++p)
                        {
                            const double fd = (a.gradient[p] - b.gradient[p]) / (2.0 * h);
                            ASSERT_NEAR(base.hessian[p * K + d], fd, 2e-5 * scale)
                                << name(dyn) << ", path " << path << ", d2 / d theta_" << p << " d theta_" << d
                                << "\n"
                                << source;
                            ASSERT_NEAR(base.hessian[p * K + d], base.hessian[d * K + p], 1e-10 * scale);
                        }
                    }
                }
            }
    }

    // The acceptance of rung 1 (§10 L6) on the IR: Black-Scholes's gamma,
    // vanna and volga from the paths' second order, where max(S - K, 0) gives
    // a gamma of exactly 0.
    TEST(ScriptIrLastStep, SecondOrderRisksAreBlackScholess)
    {
        const SecondOrderCase sc(kScripts[0], Dyn::BlackScholes, {}, kSeed);
        const std::vector<double> theta = sc.theta(); // spot, rate, div, vol
        const double S = theta[0], r = theta[1], q = theta[2], vol = theta[3], K = 100.0;
        const double T = sc.c.dm.t.back(), sqT = std::sqrt(T);
        const double d1 = (std::log(S / K) + (r - q + 0.5 * vol * vol) * T) / (vol * sqT), d2 = d1 - vol * sqT;
        const double n1 = norm_pdf(d1);
        const double gamma = std::exp(-q * T) * n1 / (S * vol * sqT);
        const double vanna = -std::exp(-q * T) * n1 * d2 / vol;
        const double volga = S * std::exp(-q * T) * n1 * sqT * d1 * d2 / vol;
        WelfordAccumulator g, va, vo;
        for (int p = 0; p < 20000; ++p)
        {
            const SecondOrder x = sc.at(theta, p);
            g.add(x.hessian[0 * 4 + 0]);
            va.add(x.hessian[0 * 4 + 3]);
            vo.add(x.hessian[3 * 4 + 3]);
        }
        EXPECT_NEAR(g.mean, gamma, 4.0 * g.std_error());
        EXPECT_NEAR(va.mean, vanna, 4.0 * va.std_error());
        EXPECT_NEAR(vo.mean, volga, 4.0 * vo.std_error());
        EXPECT_LT(g.std_error(), 0.02 * gamma);

        const SecondOrderCase kink(kMid + "2027-06-01\n    pays max(spot() - 100, 0)\n", Dyn::BlackScholes, {}, kSeed);
        double kink_gamma = 0.0;
        for (int p = 0; p < 500; ++p)
            kink_gamma += kink.at(theta, p).hessian[0];
        EXPECT_EQ(kink_gamma, 0.0);
    }

    // A bridge survival is still not in the paths, and says so.
    TEST(ScriptIrLastStep, ABridgeSurvivalIsStillRefused)
    {
        const ScriptedProduct<Real> product(kMid + "2027-06-01\n    pays survivalAbove(80) * lastStepCall(100)\n",
                                            kCtx);
        EXPECT_TRUE(product.ir().has_bridge);
        auto model = make_model<Real>(Dyn::BlackScholes, 1);
        std::string why;
        EXPECT_FALSE(simulate_script_aad_ir(product, *model, 64, kSeed, why).has_value());
        EXPECT_NE(why.find("survival"), std::string::npos) << why;
    }

    // WP 27 §14.10: a payoff that jumps (a crisp comparison of the spot)
    // has a pathwise delta of 0 on every path, so the bump on the adjoint
    // would show 0 +- 0. The engine refuses it, before any GPU work, and says
    // what to do; a fuzzy digital and lastStepDigital are not refused for it.
    TEST(ScriptIrLastStep, TheSecondOrderRefusesAPayoffThatJumps)
    {
        auto model = make_model<Real>(Dyn::BlackScholes, 1);
        std::string why;
        const ScriptedProduct<Real> hard("2027-06-01\n    if spot() > 100 then pays 1 endIf\n", kCtx);
        EXPECT_TRUE(hard.analysis().spot_comparison);
        EXPECT_FALSE(simulate_script_second_order_gpu(hard, *model, 1024, kSeed, why).has_value());
        EXPECT_NE(why.find("jumps"), std::string::npos) << why;
        EXPECT_NE(why.find("lastStepDigital"), std::string::npos) << why;

        ScriptSettings fuzzy;
        fuzzy.fuzzy = true;
        const ScriptedProduct<Real> smoothed("2027-06-01\n    if spot() > 100 then pays 1 endIf\n", kCtx, fuzzy);
        why.clear();
        simulate_script_second_order_gpu(smoothed, *model, 1024, kSeed, why);
        EXPECT_EQ(why.find("jumps"), std::string::npos) << why;

        const ScriptedProduct<Real> last("2027-06-01\n    pays lastStepDigital(100)\n", kCtx);
        EXPECT_FALSE(last.analysis().spot_comparison);
        why.clear();
        simulate_script_second_order_gpu(last, *model, 1024, kSeed, why);
        EXPECT_EQ(why.find("jumps"), std::string::npos) << why;
    }

} // namespace quantModeling
