#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;

        const Date kToday = Date::from_iso("2024-06-03");

        ValuationContext ctx() { return ValuationContext{kToday}; }

        ScriptModelSpec black_scholes(double spot, double rate, double vol, double dividend = 0.0)
        {
            ScriptModelSpec s;
            s.spot = spot;
            s.rate = rate;
            s.vol = vol;
            s.dividend = dividend;
            return s;
        }

        struct Priced
        {
            SimulationMCResult mc;
            LsmReport lsm;
        };

        Priced price(const std::string &script, const ScriptModelSpec &spec, int paths = 100000)
        {
            ScriptedProduct<Real> product(script, ctx());
            auto model = make_script_model<Real>(spec);
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 7;
            Priced out;
            out.lsm = fit_exercise_policy(product, *model, paths, 7);
            out.mc = simulate<Real>(product, *model, s);
            return out;
        }

        /// Every `step` days from the day after today to `last` (inclusive).
        std::vector<Date> dates_every(int step, int count)
        {
            std::vector<Date> out;
            for (int i = 1; i <= count; ++i)
                out.push_back(kToday + step * i);
            return out;
        }

        std::string date_line(const std::vector<Date> &dates)
        {
            std::string line;
            for (const Date &d : dates)
                line += d.to_iso() + " ";
            return line;
        }

        /// Bermudan put on a CRR tree, exercisable only at `times` — the
        /// reference the regression must approach from below.
        double bermudan_put_tree(double S, double K, double r, double vol, const std::vector<double> &times,
                                 int steps)
        {
            const double T = times.back(), dt = T / steps;
            const double u = std::exp(vol * std::sqrt(dt)), d = 1.0 / u;
            const double p = (std::exp(r * dt) - d) / (u - d), disc = std::exp(-r * dt);
            std::vector<bool> exercisable(static_cast<std::size_t>(steps + 1), false);
            for (const double t : times)
                exercisable[static_cast<std::size_t>(std::lround(t / dt))] = true;
            std::vector<double> v(static_cast<std::size_t>(steps + 1));
            for (int j = 0; j <= steps; ++j)
                v[static_cast<std::size_t>(j)] = std::max(K - S * std::pow(u, j) * std::pow(d, steps - j), 0.0);
            for (int n = steps - 1; n >= 0; --n)
                for (int j = 0; j <= n; ++j)
                {
                    const auto J = static_cast<std::size_t>(j);
                    double cont = disc * (p * v[J + 1] + (1 - p) * v[J]);
                    if (exercisable[static_cast<std::size_t>(n)])
                        cont = std::max(cont, K - S * std::pow(u, j) * std::pow(d, n - j));
                    v[J] = cont;
                }
            return v[0];
        }

        double bs(bool call, double S, double K, double T, double r, double q, double vol)
        {
            const double sd = vol * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q) * T + 0.5 * sd * sd) / sd, d2 = d1 - sd;
            return call ? S * std::exp(-q * T) * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d2)
                        : K * std::exp(-r * T) * norm_cdf(-d2) - S * std::exp(-q * T) * norm_cdf(-d1);
        }
    } // namespace

    TEST(Lsm, ParsesExerciseAndCallAsConditions)
    {
        EXPECT_NO_THROW(scripting::parse_script("2025-01-02\n    if exercise(spot(), spot()*spot()) then\n"
                                                "        pays 1\n    endIf\n"));
        EXPECT_NO_THROW(scripting::parse_script("2025-01-02\n    if not call() then\n        pays 1\n    endIf\n"));
        // A script with both rights is refused: one side decides.
        EXPECT_THROW(ScriptedProduct<Real>("2025-01-02\n    if exercise() then\n        pays 1\n    endIf\n"
                                           "2025-02-03\n    if call() then\n        pays 1\n    endIf\n",
                                           ctx()),
                     InvalidInput);
    }

    TEST(Lsm, PricingWithoutAFittedRuleIsAnError)
    {
        ScriptedProduct<Real> product("2025-06-03\n    if exercise() then\n        pays 1\n    endIf\n", ctx());
        auto model = make_script_model<Real>(black_scholes(100, 0.02, 0.2));
        PricingSettings s;
        s.mc_paths = 100;
        EXPECT_THROW(simulate<Real>(product, *model, s), InvalidInput);
    }

    TEST(Lsm, BermudanPutApproachesTheTreeFromBelow)
    {
        // Longstaff & Schwartz (2001), table 1: S = 36, K = 40, r = 6 %,
        // sigma = 20 %, one year; here every two weeks. As they advise, the
        // exercise value is a regressor: with spot() alone the rule is 5
        // cents short, with the payoff too, under one.
        const std::vector<Date> dates = dates_every(14, 26);
        const std::string script = date_line(dates) + "\n    if exercise(max(40 - spot(), 0), spot()) then\n"
                                                      "        pays max(40 - spot(), 0)\n    endIf\n";
        const Priced r = price(script, black_scholes(36.0, 0.06, 0.2), 200000);
        std::vector<double> times;
        for (const Date &d : dates)
            times.push_back((d - kToday) / 365.0);
        const double tree = bermudan_put_tree(36.0, 40.0, 0.06, 0.2, times, 26 * 60);
        const double se = r.mc.std_error();
        EXPECT_LT(r.mc.npv(), tree + 3.0 * se); // a lower bound, up to noise
        EXPECT_GT(r.mc.npv(), tree - 0.02);
        EXPECT_GT(r.mc.npv(), bs(false, 36.0, 40.0, times.back(), 0.06, 0.0, 0.2) + 0.2); // early-exercise premium
        EXPECT_EQ(r.lsm.dates.size(), 26u);
        EXPECT_EQ(r.lsm.regressors, 2u);
    }

    TEST(Lsm, AnAmericanCallWithoutDividendIsTheEuropean)
    {
        // Never optimal to exercise early: the rule must not destroy value.
        const std::vector<Date> dates = dates_every(30, 12);
        const std::string script = date_line(dates) + "\n    if exercise(spot()) then\n"
                                                      "        pays max(spot() - 100, 0)\n    endIf\n";
        const Priced r = price(script, black_scholes(100.0, 0.05, 0.25));
        const double european = bs(true, 100.0, 100.0, (dates.back() - kToday) / 365.0, 0.05, 0.0, 0.25);
        EXPECT_NEAR(r.mc.npv(), european, 4.0 * r.mc.std_error() + 0.02);
    }

    TEST(Lsm, SimpleChooserMatchesRubinstein)
    {
        // At t1 the holder picks a call or a put on K at T (Rubinstein 1991):
        // V = C(K, T) + e^{-q(T - t1)} P(S, K e^{-(r - q)(T - t1)}, t1).
        const Date t1 = kToday + 182, T = kToday + 365;
        const std::string script = t1.to_iso() + "\n    if exercise(spot()) then\n        is_call = 1\n    endIf\n" +
                                   T.to_iso() +
                                   "\n    if is_call = 1 then\n        pays max(spot() - 100, 0)\n    else\n"
                                   "        pays max(100 - spot(), 0)\n    endIf\n";
        const double r = 0.04, q = 0.01, vol = 0.3, S = 100.0;
        const double tt1 = 182 / 365.0, TT = 1.0;
        const Priced p = price(script, black_scholes(S, r, vol, q), 200000);
        const double exact =
            bs(true, S, 100.0, TT, r, q, vol) +
            std::exp(-q * (TT - tt1)) * bs(false, S, 100.0 * std::exp(-(r - q) * (TT - tt1)), tt1, r, q, vol);
        EXPECT_NEAR(p.mc.npv(), exact, 4.0 * p.mc.std_error() + 0.02);
    }

    TEST(Lsm, AnIssuerCallIsWorthLessToTheHolderThanNoCallAndAHolderPutMore)
    {
        // A 4-year 5 % note on an equity, capital at risk at maturity. The
        // issuer may redeem at par on each coupon date (call); the mirror
        // gives the holder that right (exercise).
        const auto note = [](const std::string &right)
        {
            std::string s = "2025-06-03 2026-06-03 2027-06-03\n    if done = 0 then\n        pays 5\n";
            if (!right.empty())
                s += "        if " + right + "(spot()) then\n            pays 100\n            done = 1\n"
                     "        endIf\n";
            return s + "    endIf\n2028-06-05\n    if done = 0 then\n"
                       "        pays 105 * min(1, spot() / 100)\n    endIf\n";
        };
        const ScriptModelSpec m = black_scholes(100.0, 0.03, 0.25);
        const Priced plain = price(note(""), m);
        const Priced callable = price(note("call"), m);
        const Priced putable = price(note("exercise"), m);
        const double tol = 3.0 * plain.mc.std_error();
        EXPECT_LT(callable.mc.npv(), plain.mc.npv() - tol);
        EXPECT_GT(putable.mc.npv(), plain.mc.npv() + tol);
        EXPECT_TRUE(plain.lsm.dates.empty());
        EXPECT_EQ(callable.lsm.dates.size(), 3u);
    }

} // namespace quantModeling
