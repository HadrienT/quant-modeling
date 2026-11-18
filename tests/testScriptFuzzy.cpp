#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr double S0 = 100, K = 100, r = 0.03, q = 0.0, v = 0.20;

        double d2(double spot)
        {
            const double T = 1.0;
            return (std::log(spot / K) + (r - q - 0.5 * v * v) * T) /
                   (v * std::sqrt(T));
        }
        double digital_price()
        {
            return 100.0 * std::exp(-r) * norm_cdf(d2(S0));
        }
        double digital_delta()
        {
            const double phi =
                std::exp(-0.5 * d2(S0) * d2(S0)) / std::sqrt(2 * M_PI);
            return 100.0 * std::exp(-r) * phi / (S0 * v * std::sqrt(1.0));
        }

        double price(const std::string &src, bool fuzzy, double eps, double spot,
                     int seed, int paths = 200000)
        {
            const ValuationContext ctx{Date::from_iso("2024-12-16")};
            ScriptedProduct<Real> product(src, ctx, {fuzzy, eps});
            BlackScholesSimModel<Real> model(spot, r, q, v);
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            s.mc_antithetic = true;
            // Pseudo-random: these tests measure the hard indicator's bump noise
            // across seeds, which Sobol (the default) all but removes.
            s.mc_sampler = SamplerKind::PseudoRandom;
            return simulate<Real>(product, model, s).npv();
        }

        const std::string kDigital =
            "2025-12-16\n    if spot() > 100 then pays 100 endIf\n";
        const std::string kCall = "2025-12-16\n    pays max(spot() - 100, 0)\n";
    } // namespace

    TEST(ScriptFuzzy, NoConditionsMeansIdenticalToHard)
    {
        // a plain call has no comparison to smooth: bit-identical, same seed
        const double hard = price(kCall, false, 0.01, S0, 42);
        const double fuzzy = price(kCall, true, 0.01, S0, 42);
        EXPECT_NEAR(hard, fuzzy, 1e-12);
    }

    TEST(ScriptFuzzy, ConvergesToHardAsEpsShrinks)
    {
        // Same seed => the fuzzy and hard runs share paths, so the difference
        // is the smoothing bias. In the wide-band regime it shrinks with a
        // measured order well above 1 (the centred call spread is ~2nd order);
        // below that a Monte-Carlo noise floor takes over, so we test the bias
        // regime only.
        const int N = 500000;
        const double hard = price(kDigital, false, 0.0, S0, 7, N);
        std::vector<double> eps{16.0, 8.0, 4.0};
        std::vector<double> err;
        for (double e : eps)
            err.push_back(std::fabs(price(kDigital, true, e, S0, 7, N) - hard));

        EXPECT_LT(err[1], err[0]);
        EXPECT_LT(err[2], err[1]);

        const double order =
            std::log(err[0] / err[2]) / std::log(eps[0] / eps[2]);
        EXPECT_GT(order, 1.5) << "measured convergence order " << order;
        EXPECT_LT(err[2], 0.3);
    }

    TEST(ScriptFuzzy, DigitalDeltaIsBoundedAndAccurate)
    {
        const double h = 1e-4; // tiny bump: a hard digital delta is unusable here
        auto delta = [&](bool fuzzy, int seed)
        {
            const double up = price(kDigital, fuzzy, 1.0, S0 + h, seed);
            const double dn = price(kDigital, fuzzy, 1.0, S0 - h, seed);
            return (up - dn) / (2 * h);
        };

        const double f1 = delta(true, 11), f2 = delta(true, 22);
        const double h1 = delta(false, 11), h2 = delta(false, 22);

        const double ref = digital_delta();                    // ~1.93
        EXPECT_NEAR(0.5 * (f1 + f2), ref, 0.4);                // fuzzy tracks the analytic
        EXPECT_LT(std::fabs(f1 - f2), 0.1);                    // and is stable across seeds
        EXPECT_GT(std::fabs(h1 - h2), 5 * std::fabs(f1 - f2)); // hard is not
    }

    TEST(ScriptFuzzy, DigitalPriceStaysCloseToHard)
    {
        const double hard = price(kDigital, false, 0.0, S0, 3);
        const double fuzzy = price(kDigital, true, 1.0, S0, 3);
        EXPECT_NEAR(fuzzy, digital_price(), 1.0);
        EXPECT_NEAR(fuzzy, hard, 1.0);
    }

    TEST(ScriptFuzzy, AutocallWithFlagsAndBarriersStaysCloseToHard)
    {
        // discrete flag tests stay crisp, the barrier tests get smoothed
        const std::string src =
            "2025-01-06\n    s0 = spot()\n    ki = 0\n    alive = 1\n"
            "2025-06-16  2025-12-16\n"
            "    if spot() < 0.70 * s0 then ki = 1 endIf\n"
            "    if alive = 1 and spot() >= s0 then\n"
            "        pays 1000\n        alive = 0\n"
            "    endIf\n"
            "2026-06-16\n"
            "    if alive = 1 then\n"
            "        if ki = 1 then pays 1000 * spot() / s0 else pays 1000 endIf\n"
            "    endIf\n";
        const double hard = price(src, false, 0.0, S0, 5);
        const double fuzzy = price(src, true, 0.5, S0, 5);
        EXPECT_NEAR(fuzzy, hard, 0.02 * hard); // within 2%
        EXPECT_GT(fuzzy, 0.0);
    }

    // A knock-out read through a flag (#226 follow-up): the fuzzy `if` blends
    // `alive` to a fraction near the barrier, and the discrete test
    // `alive = 1` interpolates (NodeComparison::discrete_eps) instead of
    // reading 0.97 as false -- which dropped the payoff of every path near
    // the barrier. The smoothed price converges to the hard one, and its
    // bump delta is the hard price's.
    TEST(ScriptFuzzy, AKnockOutReadThroughAFlagKeepsItsPayoff)
    {
        const std::string src = "2024-12-17\n    alive = 1\n"
                                "schedule(2025-01-16, 2025-11-17, 1M, NONE, F)\n"
                                "    if spot() >= 130 then alive = 0 endIf\n"
                                "2025-12-16\n"
                                "    if spot() >= 130 then alive = 0 endIf\n"
                                "    if alive = 1 then pays max(spot() - 100, 0) endIf\n";
        const double hard = price(src, false, 0.0, S0, 7);
        double gap = 1e300;
        for (const double eps : {2.0, 0.5, 0.1})
        {
            const double fuzzy = price(src, true, eps, S0, 7);
            const double now = std::abs(fuzzy - hard);
            EXPECT_LT(now, 0.01 * hard) << "eps " << eps;
            EXPECT_LE(now, gap + 0.002 * hard) << "eps " << eps; // no worse as eps shrinks
            gap = now;
        }
        const double h = 1.0;
        const double bump_hard = (price(src, false, 0.0, S0 + h, 7) - price(src, false, 0.0, S0 - h, 7)) / (2 * h);
        const double bump_fuzzy = (price(src, true, 0.5, S0 + h, 7) - price(src, true, 0.5, S0 - h, 7)) / (2 * h);
        EXPECT_NEAR(bump_fuzzy, bump_hard, 0.05 * std::abs(bump_hard));
    }

    TEST(ScriptFuzzy, Deterministic)
    {
        EXPECT_EQ(price(kDigital, true, 1.0, S0, 99),
                  price(kDigital, true, 1.0, S0, 99));
    }

} // namespace quantModeling
