#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/stats.hpp"

#include "ir_fixtures.hpp"

// WP 27 §8 rung 3, §14.11: Vibrato -- pathwise up to the last step, a
// likelihood ratio over it -- gives a payoff that jumps at its final date
// (a hard digital, whose pathwise delta is 0 on every path) its greeks
// without smoothing it. On the host (the CI has no GPU):
//   - the digital's delta and vega against Black-Scholes's closed forms, and
//     a call's, on one and on two steps;
//   - gamma, vanna and volga (central differences of the Vibrato delta and
//     vega on the same paths) against the closed forms, through the
//     dispatcher, which picks Vibrato for a hard payoff that jumps;
//   - local vol on a flat surface is Black-Scholes; on a skewed one the
//     delta equals the central difference of the price on the same paths;
//   - Heston and SLV are refused: their last step's variance comes near 0,
//     where the weight Z / s has no finite variance;
//   - what Vibrato does not cover says why, including a digital struck at a
//     later fixing, whose price does not depend on today's spot.

namespace quantModeling
{
    namespace
    {
        using namespace ir_test;
        namespace ir = scripting::ir;

        constexpr std::uint64_t kSeed = 29;
        // make_model's Black-Scholes: S = 100, r = 3 %, q = 1 %, vol = 25 %;
        // kCtx is 2026-06-01, so 2027-06-01 is T = 1.
        constexpr double S0 = 100.0, R = 0.03, Q = 0.01, VOL = 0.25, T = 1.0;

        const std::string kDigital = "2027-06-01\n    if spot() > 100 then pays 1 endIf\n";
        const std::string kDigitalTwoSteps = "2026-12-01\n    a = spot()\n" + kDigital;
        const std::string kCall = "2027-06-01\n    pays max(spot() - 100, 0)\n";

        struct Bs
        {
            double d1, d2, df;
            Bs(double S, double vol)
            {
                d1 = (std::log(S / 100.0) + (R - Q + 0.5 * vol * vol) * T) / (vol * std::sqrt(T));
                d2 = d1 - vol * std::sqrt(T);
                df = std::exp(-R * T);
            }
        };
        double digital_delta(double S, double vol)
        {
            const Bs b(S, vol);
            return b.df * norm_pdf(b.d2) / (S * vol * std::sqrt(T));
        }
        double digital_vega(double S, double vol)
        {
            const Bs b(S, vol);
            return -b.df * norm_pdf(b.d2) * b.d1 / vol;
        }

        /// The Vibrato estimates' means and errors over `paths`.
        std::vector<WelfordAccumulator> vibrato(const std::string &source, Dyn dyn, int paths)
        {
            const Case c(source, dyn, ScriptSettings{});
            const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, 1, true, true);
            const ir::PathProgram tan = ir::tangent(primal, all_risks(primal.layout));
            const ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
            const int K = primal.layout.n_risks;
            std::vector<WelfordAccumulator> acc(static_cast<std::size_t>(K));
            std::vector<double> d(static_cast<std::size_t>(K));
            ir::DerivativeScratch w;
            mc::PathDraws draws;
            draws.seed = kSeed;
            draws.stride = c.host->view.stride;
            for (int p = 0; p < paths; ++p)
            {
                draws.begin(static_cast<std::uint64_t>(p));
                ir::run_path_vibrato(tan, K, data, c.dm, c.product.ir(), draws, w, d.data());
                for (int k = 0; k < K; ++k)
                    acc[static_cast<std::size_t>(k)].add(d[static_cast<std::size_t>(k)]);
            }
            return acc;
        }
    } // namespace

    TEST(ScriptIrVibrato, AHardDigitalsDeltaAndVegaAreBlackScholess)
    {
        for (const std::string &source : {kDigital, kDigitalTwoSteps})
        {
            const auto acc = vibrato(source, Dyn::BlackScholes, 100000);
            const double delta = digital_delta(S0, VOL), vega = digital_vega(S0, VOL);
            EXPECT_NEAR(acc[0].mean, delta, 4.0 * acc[0].std_error()) << source;
            EXPECT_NEAR(acc[3].mean, vega, 4.0 * acc[3].std_error()) << source;
            EXPECT_LT(acc[0].std_error(), 0.02 * delta) << source;
        }
    }

    TEST(ScriptIrVibrato, ACallsDeltaIsBlackScholess)
    {
        const auto acc = vibrato(kCall, Dyn::BlackScholes, 100000);
        const Bs b(S0, VOL);
        const double delta = std::exp(-Q * T) * norm_cdf(b.d1);
        EXPECT_NEAR(acc[0].mean, delta, 4.0 * acc[0].std_error());
    }

    TEST(ScriptIrVibrato, TheDigitalsSecondOrderIsBlackScholessThroughTheDispatcher)
    {
        // The closed forms' second derivatives by central differences of
        // the closed-form delta and vega (deterministic, h = 1e-4).
        const double h = 1e-4;
        const double gamma = (digital_delta(S0 + h, VOL) - digital_delta(S0 - h, VOL)) / (2.0 * h);
        const double vanna = (digital_vega(S0 + h, VOL) - digital_vega(S0 - h, VOL)) / (2.0 * h);
        const double volga = (digital_vega(S0, VOL + h) - digital_vega(S0, VOL - h)) / (2.0 * h);
        for (const std::string &source : {kDigital, kDigitalTwoSteps})
        {
            const ScriptedProduct<Real> product(source, kCtx);
            auto model = make_model<Real>(Dyn::BlackScholes, 1);
            std::string why;
            const auto so = simulate_script_second_order(product, *model, 200000, kSeed, why);
            ASSERT_TRUE(so.has_value()) << why;
            EXPECT_NE(so->method.find("Vibrato"), std::string::npos) << so->method;
            // the bumps' bias is O(h^2): 1-2 % on top of the error
            EXPECT_NEAR(so->gamma, gamma, 4.0 * so->gamma_std_error + 0.01 * std::abs(gamma)) << source;
            ASSERT_TRUE(so->vanna.has_value());
            EXPECT_NEAR(*so->vanna, vanna, 4.0 * *so->vanna_std_error + 0.02 * std::abs(vanna)) << source;
            EXPECT_NEAR(*so->volga, volga, 4.0 * *so->volga_std_error + 0.02 * std::abs(volga)) << source;
            EXPECT_LT(so->gamma_std_error, 0.1 * std::abs(gamma)) << source;
        }
    }

    TEST(ScriptIrVibrato, FlatLocalVolIsBlackScholes)
    {
        std::vector<Real> flat(kK.size() * kT.size(), VOL);
        const ScriptedProduct<Real> product(kDigital, kCtx);
        LocalVolSimModel<Real> model(S0, R, Q, kK, kT, flat, 1.0 / 24.0);
        std::string why;
        const auto so = simulate_script_second_order(product, model, 100000, kSeed, why);
        ASSERT_TRUE(so.has_value()) << why;
        const double h = 1e-4;
        const double gamma = (digital_delta(S0 + h, VOL) - digital_delta(S0 - h, VOL)) / (2.0 * h);
        EXPECT_NEAR(so->gamma, gamma, 4.0 * so->gamma_std_error + 0.01 * std::abs(gamma));
        EXPECT_FALSE(so->vanna.has_value()); // a grid, not one vol
    }

    // No closed form under a skewed local vol: the Vibrato delta against the
    // central difference of the (Vibrato) price on the same paths.
    TEST(ScriptIrVibrato, UnderSkewedLocalVolTheDeltaIsThePricesDerivative)
    {
        const Case c(kDigitalTwoSteps, Dyn::LocalVol, ScriptSettings{});
        const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, 1, true, true);
        const ir::PathProgram tan = ir::tangent(primal, all_risks(primal.layout));
        ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
        ir::PathData up = data, down = data;
        const double h = 1.0;
        up.theta[0] += h;
        down.theta[0] -= h;
        const int K = primal.layout.n_risks;
        std::vector<double> d(static_cast<std::size_t>(K)), scratch(static_cast<std::size_t>(K));
        WelfordAccumulator delta, fd;
        ir::DerivativeScratch w;
        mc::PathDraws draws;
        draws.seed = kSeed;
        draws.stride = c.host->view.stride;
        for (int p = 0; p < 40000; ++p)
        {
            draws.begin(static_cast<std::uint64_t>(p));
            ir::run_path_vibrato(tan, K, data, c.dm, c.product.ir(), draws, w, d.data());
            delta.add(d[0]);
            draws.begin(static_cast<std::uint64_t>(p));
            const double pu = ir::run_path_vibrato(tan, K, up, c.dm, c.product.ir(), draws, w, scratch.data());
            draws.begin(static_cast<std::uint64_t>(p));
            const double pd = ir::run_path_vibrato(tan, K, down, c.dm, c.product.ir(), draws, w, scratch.data());
            fd.add((pu - pd) / (2.0 * h));
        }
        EXPECT_GT(delta.mean, 0.0);
        EXPECT_NEAR(delta.mean, fd.mean, 4.0 * std::hypot(delta.std_error(), fd.std_error()));
        EXPECT_LT(delta.std_error(), 0.05 * delta.mean);
    }

    TEST(ScriptIrVibrato, WhatItDoesNotCoverSaysWhy)
    {
        auto model = make_model<Real>(Dyn::BlackScholes, 1);
        std::string why;
        // a barrier observed at an earlier date: a jump Vibrato's pathwise part misses
        const ScriptedProduct<Real> barrier("2026-12-01\n    if spot() < 80 then out = 1 endIf\n"
                                            "2027-06-01\n    if out = 0 then pays max(spot() - 100, 0) endIf\n",
                                            kCtx);
        EXPECT_FALSE(simulate_script_second_order(barrier, *model, 1000, kSeed, why).has_value());
        EXPECT_NE(why.find("before its final date"), std::string::npos) << why;
        // two underlyings
        auto two = make_model<Real>(Dyn::BlackScholes, 2);
        const ScriptedProduct<Real> spread("2027-06-01\n    if spot(0) > spot(1) then pays 1 endIf\n", kCtx);
        why.clear();
        EXPECT_FALSE(simulate_script_second_order(spread, *two, 1000, kSeed, why).has_value());
        EXPECT_NE(why.find("one underlying"), std::string::npos) << why;
        // a stochastic variance: the weight Z / s has no finite variance near v = 0
        auto heston = make_model<Real>(Dyn::Heston, 1);
        const ScriptedProduct<Real> digital(kDigital, kCtx);
        why.clear();
        EXPECT_FALSE(simulate_script_second_order(digital, *heston, 1000, kSeed, why).has_value());
        EXPECT_NE(why.find("finite variance"), std::string::npos) << why;
    }

    // The scripting library's digital call: struck at 100 % of the spot at a
    // fixing after today, so its price does not move with today's spot and
    // its gamma is 0. Vibrato's ratio over the last step ignores that the
    // strike moves with the path and gave the gamma of a fixed-strike digital
    // instead (-0.019 on the site): the dispatcher must refuse it.
    TEST(ScriptIrVibrato, AStrikeFixedFromTheSpotLaterIsRefusedNotMispriced)
    {
        auto model = make_model<Real>(Dyn::BlackScholes, 1);
        const std::string forward = "2026-07-01\n    s0 = spot()\n"
                                    "2027-06-01\n    if spot() > s0 then pays 1 endIf\n";
        const ScriptedProduct<Real> product(forward, kCtx);
        ASSERT_TRUE(product.analysis().path_dependent);
        std::string why;
        EXPECT_FALSE(simulate_script_second_order(product, *model, 1000, kSeed, why).has_value());
        EXPECT_NE(why.find("earlier date"), std::string::npos) << why;
        // An earlier spot that is assigned but never read again changes nothing.
        const ScriptedProduct<Real> unread(kDigitalTwoSteps, kCtx);
        EXPECT_FALSE(unread.analysis().path_dependent);
    }

} // namespace quantModeling
