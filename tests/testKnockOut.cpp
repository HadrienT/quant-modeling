// knockOutAbove(L) / knockOutBelow(L) (issue #369): a knock-out at a date
// that the model handles by drawing the step into the date conditioned on
// survival and carrying the survival probability as a weight (Glasserman &
// Staum 2001). The payoff has no indicator left.
//
// Properties checked:
//  - the language: parsing, writing back, the statement only at the top of
//    a date; a level known today (a number, a term, a fixing taken) is
//    conditioned on, one that is not makes every knock-out its threshold;
//  - unbiased: one date under Black-Scholes reprices the closed form of a
//    call knocked out at expiry; twelve monthly dates agree with the hard
//    script (the `alive` flag) within error, with a smaller error;
//  - a corridor (above and below at the same dates) agrees with the hard
//    double barrier;
//  - payments before the first knock-out are not weighted;
//  - local vol on a skewed surface agrees with the hard script;
//  - the adjoint delta, with no smoothing, is the bumped price's;
//  - Heston and SLV, Euler and QE, agree with the hard script; the
//    Heston adjoint delta (Feller holding) is the bumped price's;
//  - the models that cannot condition refuse it.

#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/knock_out.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/scripting/model_advice.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/script_writer.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using aad::Tape;

        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        constexpr double S0 = 100.0, r = 0.05, q = 0.02, vol = 0.2;
        const Date kToday = Date::from_iso("2024-06-03");

        ValuationContext ctx()
        {
            return ValuationContext{kToday};
        }

        PricingSettings settings(int paths = 200000, int seed = 20240603)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            return s;
        }

        const std::map<Date, std::vector<double>> kFixing{{kToday, {S0}}};

        double bs_call(double k, double T)
        {
            const double sd = vol * std::sqrt(T);
            const double d1 = (std::log(S0 / k) + (r - q + 0.5 * vol * vol) * T) / sd;
            return S0 * std::exp(-q * T) * norm_cdf(d1) - k * std::exp(-r * T) * norm_cdf(d1 - sd);
        }
        double bs_digital(double k, double T)
        {
            const double sd = vol * std::sqrt(T);
            const double d2 = (std::log(S0 / k) + (r - q - 0.5 * vol * vol) * T) / sd;
            return std::exp(-r * T) * norm_cdf(d2);
        }

        const char *kMonths[] = {"2024-07-03", "2024-08-03", "2024-09-03", "2024-10-03",
                                 "2024-11-03", "2024-12-03", "2025-01-03", "2025-02-03",
                                 "2025-03-03", "2025-04-03", "2025-05-03", "2025-06-03"};

        /// An up-and-out call on monthly closes, strike and barrier struck
        /// today: conditioned (knockOutAbove) or hard (an `alive` flag).
        std::string up_and_out(bool conditioned, double barrier = 1.2, bool corridor = false)
        {
            std::string s = "2024-06-03\n    s0 = spot()\n    barrier = " + std::to_string(barrier) + "\n";
            if (!conditioned)
                s += "    alive = 1\n";
            for (const char *d : kMonths)
            {
                s += std::string(d) + "\n";
                if (conditioned)
                {
                    s += "    knockOutAbove(barrier * s0)\n";
                    if (corridor)
                        s += "    knockOutBelow(0.85 * s0)\n";
                }
                else
                {
                    s += "    if spot() >= barrier * s0 then alive = 0 endIf\n";
                    if (corridor)
                        s += "    if spot() <= 0.85 * s0 then alive = 0 endIf\n";
                }
            }
            s += conditioned ? "    pays max(spot() - s0, 0)\n" : "    if alive = 1 then pays max(spot() - s0, 0) endIf\n";
            return s;
        }

        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};
        std::vector<Real> skew()
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (std::size_t j = 0; j < kT.size(); ++j)
                    g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0);
            return g;
        }
    } // namespace

    TEST(KnockOut, ParsesWritesBackAndIsAStatementOfItsOwn)
    {
        // The writer's fixed point: parse(write(parse(s))) writes back the same.
        const std::string src = "2025-06-03\n    knockOutAbove(120)\n    knockOutBelow(0.8 * k)\n    pays 1\n";
        const std::string written = scripting::ScriptWriter::write(scripting::parse_script(src));
        EXPECT_EQ(scripting::ScriptWriter::write(scripting::parse_script(written)), written);
        EXPECT_NE(written.find("knockOutAbove(120"), std::string::npos);
        EXPECT_NE(written.find("knockOutBelow("), std::string::npos);

        EXPECT_THROW(ScriptedProduct<Real>("2025-06-03\n    if spot() > 1 then knockOutAbove(120) endIf\n    pays 1\n",
                                           ctx()),
                     InvalidInput);
        EXPECT_THROW(scripting::parse_script("2025-06-03\n    x = knockOutAbove(120)\n"), std::exception);
    }

    TEST(KnockOut, ALevelNotKnownTodayIsPricedAsTheThreshold)
    {
        // A term and a fixing already taken: barrier * s0 = 120, conditioned.
        ScriptedProduct<Real> ok(up_and_out(true), ctx(), {}, kFixing);
        EXPECT_FALSE(ok.analysis().knock_outs_on_thresholds);
        for (const SampleDef &d : ok.defline())
            for (const StepMonitor &m : d.monitors)
                if (m.kind == StepQuantity::KnockOut)
                    EXPECT_DOUBLE_EQ(m.level, 120.0);
        // A term set on the date itself, after the statement as written.
        EXPECT_FALSE(ScriptedProduct<Real>("2025-06-03\n    knockOutAbove(b)\n    b = 120\n    pays 1\n", ctx())
                         .analysis()
                         .knock_outs_on_thresholds);

        // A spot still to be simulated, a variable set after the date: the
        // model cannot condition on them -- every knock-out is its
        // threshold (no model quantity left), the analysis says so, and the
        // price is the hard script's (the same draws, the same bits).
        for (const std::string src : {"2025-01-03\n    s1 = spot()\n2025-06-03\n    knockOutAbove(1.2 * s1)\n"
                                      "    pays max(spot() - s1, 0)\n",
                                      "2025-01-03\n    knockOutAbove(b)\n2025-06-03\n    b = 120\n    pays 1\n"})
        {
            ScriptedProduct<Real> forward(src, ctx());
            EXPECT_TRUE(forward.analysis().knock_outs_on_thresholds) << src;
            for (const SampleDef &d : forward.defline())
                EXPECT_TRUE(d.monitors.empty()) << src;
            const auto advice = scripting::advise(forward.analysis(), scripting::ModelKind::LocalVolSurface, 1.0, 2.0);
            EXPECT_TRUE(std::any_of(advice.begin(), advice.end(), [](const scripting::Advice &a)
                                    { return a.code == "knock_outs_as_thresholds"; }))
                << src;
        }
        ScriptedProduct<Real> forward("2025-01-03\n    s1 = spot()\n2025-06-03\n    knockOutAbove(1.2 * s1)\n"
                                      "    pays max(spot() - s1, 0)\n",
                                      ctx());
        ScriptedProduct<Real> hard("2025-01-03\n    s1 = spot()\n2025-06-03\n"
                                   "    if spot() < 1.2 * s1 then pays max(spot() - s1, 0) endIf\n",
                                   ctx());
        BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
        EXPECT_EQ(simulate<Real>(forward, m1, settings(20000)).npv(), simulate<Real>(hard, m2, settings(20000)).npv());

        // A knock-out on a past date is an error.
        EXPECT_THROW(ScriptedProduct<Real>("2024-06-03\n    knockOutAbove(120)\n2025-06-03\n    pays 1\n", ctx(), {},
                                           kFixing),
                     InvalidInput);
    }

    // One date: E[(S - K)+ 1{S < H}] = C(K) - C(H) - (H - K) D(H).
    TEST(KnockOut, OneDateRepricesTheClosedForm)
    {
        ScriptedProduct<Real> prod("2025-06-03\n    knockOutAbove(120)\n    pays max(spot() - 100, 0)\n", ctx());
        const double T = prod.timeline().front();
        BlackScholesSimModel<Real> model(S0, r, q, vol);
        const auto res = simulate<Real>(prod, model, settings());
        const double exact = bs_call(100.0, T) - bs_call(120.0, T) - 20.0 * bs_digital(120.0, T);
        EXPECT_NEAR(res.npv(), exact, 4.0 * res.std_error()) << "closed form " << exact;
    }

    TEST(KnockOut, MonthlyDatesAgreeWithTheHardScriptWithASmallerError)
    {
        for (const bool corridor : {false, true})
        {
            ScriptedProduct<Real> soft(up_and_out(true, 1.2, corridor), ctx(), {}, kFixing);
            ScriptedProduct<Real> hard(up_and_out(false, 1.2, corridor), ctx(), {}, kFixing);
            BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
            const auto a = simulate<Real>(soft, m1, settings());
            const auto b = simulate<Real>(hard, m2, settings(200000, 7));
            EXPECT_NEAR(a.npv(), b.npv(), 4.0 * std::hypot(a.std_error(), b.std_error())) << "corridor " << corridor;
            EXPECT_LT(a.std_error(), b.std_error()) << "corridor " << corridor;
            EXPECT_GT(a.npv(), 0.5); // the barrier leaves something
        }
    }

    TEST(KnockOut, PaymentsBeforeTheFirstKnockOutAreNotWeighted)
    {
        // A sure coupon of 1 on the first month, then a knock-out that
        // almost surely happens: the coupon keeps its full value.
        ScriptedProduct<Real> prod("2024-07-03\n    pays 1\n2025-06-03\n    knockOutAbove(1)\n    pays 1\n", ctx());
        BlackScholesSimModel<Real> model(S0, r, q, vol);
        const auto res = simulate<Real>(prod, model, settings(20000));
        const double t1 = prod.timeline().front();
        EXPECT_NEAR(res.npv(), std::exp(-r * t1), 1e-6);
    }

    TEST(KnockOut, SkewedLocalVolAgreesWithTheHardScript)
    {
        ScriptedProduct<Real> soft(up_and_out(true), ctx(), {}, kFixing);
        ScriptedProduct<Real> hard(up_and_out(false), ctx(), {}, kFixing);
        LocalVolSimModel<Real> m1(S0, r, q, kK, kT, skew(), 1.0 / 252.0), m2(S0, r, q, kK, kT, skew(), 1.0 / 252.0);
        m1.set_refinement(2);
        m2.set_refinement(2);
        const auto a = simulate<Real>(soft, m1, settings(100000));
        const auto b = simulate<Real>(hard, m2, settings(100000, 7));
        EXPECT_NEAR(a.npv(), b.npv(), 4.0 * std::hypot(a.std_error(), b.std_error()));
        EXPECT_LT(a.std_error(), b.std_error());
    }

    // No smoothing: the conditioned path is smooth in the spot, so the
    // adjoint's delta is the derivative of the price -- here against the
    // central difference of the price on common draws.
    TEST(KnockOut, TheAdjointDeltaIsTheBumpedPrices)
    {
        const std::string script = "2025-06-03\n    knockOutAbove(120)\n    pays max(spot() - 100, 0)\n";
        const std::string monthly = [&]
        {
            std::string s;
            for (const char *d : kMonths)
                s += std::string(d) + "\n    knockOutAbove(140)\n";
            return s + "    pays max(spot() - 100, 0)\n";
        }();
        for (const std::string &src : {script, monthly})
        {
            Tape tape;
            TapeSwitch guard(tape);
            ScriptedProduct<Number> prod(src, ctx());
            BlackScholesSimModel<Number> model{Number(S0), Number(r), Number(q), Number(vol)};
            const AADSimulResults res = simulate_aad(prod, model, 50000, 11);
            ASSERT_EQ(res.risk_labels[0], "spot");

            const auto price_at = [&](double s)
            {
                Tape t2;
                TapeSwitch g2(t2);
                ScriptedProduct<Number> p(src, ctx());
                BlackScholesSimModel<Number> m{Number(s), Number(r), Number(q), Number(vol)};
                return simulate_aad(p, m, 50000, 11).price;
            };
            const double h = 1e-3;
            const double bumped = (price_at(S0 + h) - price_at(S0 - h)) / (2.0 * h);
            // Common draws: the two prices differ path by path by the
            // derivative, except where a path crosses the call's kink within
            // +-h -- an O(h) term, hence the tolerance.
            EXPECT_NEAR(res.risks[0], bumped, 2e-4 * std::abs(bumped) + 1e-6)
                << "adjoint " << res.risks[0] << " +- " << res.risk_std_errors[0] << ", price " << res.price;
            EXPECT_GT(std::abs(res.risks[0]), 10.0 * res.risk_std_errors[0]); // a delta, not a zero
        }
    }

    TEST(KnockOut, TheLocalVolAdjointDeltaIsTheBumpedPrices)
    {
        std::string src;
        for (const char *d : kMonths)
            src += std::string(d) + "\n    knockOutAbove(140)\n";
        src += "    pays max(spot() - 100, 0)\n";
        const auto run = [&](double s)
        {
            Tape tape;
            TapeSwitch guard(tape);
            ScriptedProduct<Number> prod(src, ctx());
            std::vector<Number> g;
            for (Real v : skew())
                g.emplace_back(v);
            LocalVolSimModel<Number> model(Number(s), Number(r), Number(q), kK, kT, g, 1.0 / 52.0);
            return simulate_aad(prod, model, 20000, 11);
        };
        const AADSimulResults res = run(S0);
        ASSERT_TRUE(std::isfinite(res.price)) << res.price;
        ASSERT_EQ(res.risk_labels[0], "spot");
        const double h = 1e-3;
        const double bumped = (run(S0 + h).price - run(S0 - h).price) / (2.0 * h);
        EXPECT_NEAR(res.risks[0], bumped, 2e-4 * std::abs(bumped) + 1e-6)
            << "adjoint " << res.risks[0] << " +- " << res.risk_std_errors[0];
        EXPECT_GT(std::abs(res.risks[0]), 10.0 * res.risk_std_errors[0]);
    }

    // Heston (Bates with no jumps) and SLV (L3): the spot's step is
    // lognormal given the variance; conditioned, both variance schemes agree
    // with the hard script on the same model.
    TEST(KnockOut, HestonAndSlvAgreeWithTheHardScript)
    {
        const double v0 = 0.04, kappa = 1.5, theta = 0.05, xi = 0.6, rho = -0.7; // Feller 0.42
        for (const auto scheme : {mc::VarianceScheme::Euler, mc::VarianceScheme::QE})
        {
            ScriptedProduct<Real> soft(up_and_out(true, 1.2, true), ctx(), {}, kFixing);
            ScriptedProduct<Real> hard(up_and_out(false, 1.2, true), ctx(), {}, kFixing);
            BatesSimModel<Real> m1(S0, r, q, v0, kappa, theta, xi, rho, 0.0, 0.0, 0.0, 1.0 / 52.0);
            BatesSimModel<Real> m2(S0, r, q, v0, kappa, theta, xi, rho, 0.0, 0.0, 0.0, 1.0 / 52.0);
            m1.set_variance_scheme(scheme);
            m2.set_variance_scheme(scheme);
            const auto a = simulate<Real>(soft, m1, settings(100000));
            const auto b = simulate<Real>(hard, m2, settings(100000, 7));
            EXPECT_NEAR(a.npv(), b.npv(), 4.0 * std::hypot(a.std_error(), b.std_error()))
                << "Heston, scheme " << static_cast<int>(scheme);
            // No error comparison here: the gain in variance is small under
            // stochastic vol, within the noise of 16 replicates' estimate.

            // SLV: a skewed leverage on the same Heston
            std::vector<Real> lev;
            for (Real k : kK)
                for (std::size_t j = 0; j < kT.size(); ++j)
                    lev.push_back(1.0 + 0.6 * (100.0 - k) / 100.0);
            SLVSimModel<Real> s1(S0, r, q, v0, kappa, theta, xi, rho, kK, kT, lev, 1.0 / 52.0);
            SLVSimModel<Real> s2(S0, r, q, v0, kappa, theta, xi, rho, kK, kT, lev, 1.0 / 52.0);
            s1.set_variance_scheme(scheme);
            s2.set_variance_scheme(scheme);
            const auto c = simulate<Real>(soft, s1, settings(100000));
            const auto d = simulate<Real>(hard, s2, settings(100000, 7));
            EXPECT_NEAR(c.npv(), d.npv(), 4.0 * std::hypot(c.std_error(), d.std_error()))
                << "SLV, scheme " << static_cast<int>(scheme);
        }
    }

    // Feller holding (2 kappa theta / xi^2 = 1.78), the Heston adjoint delta
    // is the bumped price's, with no smoothing.
    TEST(KnockOut, TheHestonAdjointDeltaIsTheBumpedPrices)
    {
        std::string src;
        for (const char *d : kMonths)
            src += std::string(d) + "\n    knockOutAbove(140)\n";
        src += "    pays max(spot() - 100, 0)\n";
        const auto run = [&](double s)
        {
            Tape tape;
            TapeSwitch guard(tape);
            ScriptedProduct<Number> prod(src, ctx());
            BatesSimModel<Number> model{Number(s), Number(r), Number(q), Number(0.04),
                                        Number(2.0), Number(0.04), Number(0.3), Number(-0.7),
                                        Number(0.0), Number(0.0), Number(0.0), 1.0 / 52.0};
            return simulate_aad(prod, model, 20000, 11);
        };
        const AADSimulResults res = run(S0);
        ASSERT_TRUE(std::isfinite(res.price));
        const double h = 1e-3;
        const double bumped = (run(S0 + h).price - run(S0 - h).price) / (2.0 * h);
        EXPECT_NEAR(res.risks[0], bumped, 2e-4 * std::abs(bumped) + 1e-6)
            << "adjoint " << res.risks[0] << " +- " << res.risk_std_errors[0];
        EXPECT_GT(std::abs(res.risks[0]), 10.0 * res.risk_std_errors[0]);
    }

    TEST(KnockOut, TheConditionedDrawIsTheTruncatedNormal)
    {
        // The draw stays on the safe side, p is the side's probability, and
        // with no barrier in reach nothing moves.
        mc::KnockOutCorridor c;
        c.hi = 110.0;
        c.slot = 0;
        for (const double z_in : {-2.0, -0.3, 0.0, 0.7, 1.5, 3.0})
        {
            double z = z_in;
            const double m = 0.01, s = 0.1;
            const double p = mc::condition_on_survival(c, std::log(100.0), m, s, z);
            const double du = (std::log(110.0) - std::log(100.0) - m) / s;
            EXPECT_NEAR(p, norm_cdf(du), 1e-15);
            EXPECT_LT(z, du);
            // u -> N(dl) + u p, inverted: N(z) = u p
            EXPECT_NEAR(norm_cdf(z), norm_cdf(z_in) * p, 1e-12);
        }
        mc::KnockOutCorridor far;
        far.hi = 1e6;
        far.slot = 0;
        double z = 0.4;
        EXPECT_NEAR(mc::condition_on_survival(far, std::log(100.0), 0.0, 0.1, z), 1.0, 1e-15);
        EXPECT_NEAR(z, 0.4, 1e-12);
    }

    TEST(KnockOut, ModelsThatCannotConditionRefuseIt)
    {
        ScriptedProduct<Real> prod("2025-06-03\n    knockOutAbove(120)\n    pays 1\n", ctx());
        EXPECT_THROW(refuse_knock_outs(prod.defline(), "RoughBergomiSimModel"), InvalidInput);
        EXPECT_THROW(refuse_step_monitors(prod.defline(), "MertonSimModel"), InvalidInput);
        EXPECT_NO_THROW(refuse_bridge_survivals(prod.defline(), "SLVSimModel"));
        // jumps make the step a mixture
        BatesSimModel<Real> bates(S0, r, q, 0.04, 2.0, 0.04, 0.3, -0.7, 0.5, -0.1, 0.1, 1.0 / 52.0);
        EXPECT_THROW(bates.init(prod.timeline(), prod.defline()), InvalidInput);
        // the analysis treats it as the barrier on dates it replaces: path-
        // dependent, not a bridge survival (no model is forced)
        EXPECT_TRUE(prod.analysis().knock_out);
        EXPECT_FALSE(prod.analysis().barrier_monitoring);
        EXPECT_TRUE(prod.analysis().path_dependent);
        EXPECT_FALSE(prod.analysis().spot_comparison);
    }

} // namespace quantModeling
