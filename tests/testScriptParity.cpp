#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/equity/simulatable_asian.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/utils/stats.hpp"

// legacy autocall engine, for the third parity
#include "quantModeling/engines/mc/autocall.hpp"
#include "quantModeling/instruments/equity/autocall.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/pricer.hpp"

// legacy Himalaya/Mountain engine and the new multi-asset sim model, for the
// 16e acceptance criterion (a multi-underlying script vs BSMountainMCEngine)
#include "quantModeling/engines/mc/mountain.hpp"
#include "quantModeling/instruments/equity/mountain.hpp"
#include "quantModeling/models/equity/multi_asset_bs_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"

#include <Eigen/Core>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        double bs_call(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            return S * std::exp(-q * T) * norm_cdf(d1) -
                   K * std::exp(-r * T) * norm_cdf(d1 - sd);
        }

        PricingSettings mc_settings()
        {
            PricingSettings s;
            s.mc_paths = 200000;
            s.mc_seed = 20240603;
            s.mc_antithetic = true;
            return s;
        }

        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2024-06-03")};
        }
    } // namespace

    // ── 1. a scripted European call vs the closed form ──────────────────────

    TEST(ScriptParity, EuropeanCallMatchesBlackScholes)
    {
        const double S0 = 100, K = 105, r = 0.03, q = 0.01, v = 0.2;
        ScriptedProduct<Real> prod("2025-06-03\n    pays max(spot() - 105, 0)\n",
                                   ctx());
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());

        const double T = prod.timeline().front();
        EXPECT_NEAR(res.npv(), bs_call(S0, K, r, q, v, T), 4.0 * res.std_error());
        EXPECT_GT(res.std_error(), 0.0);
    }

    // ── 2. a scripted arithmetic Asian vs SimulatableAsian, same paths ──────

    TEST(ScriptParity, ArithmeticAsianMatchesSimulatableAsianPathForPath)
    {
        const double S0 = 100, K = 100, r = 0.05, q = 0.0, v = 0.25;
        const std::string src =
            "2024-09-01\n    acc = spot()\n"
            "2024-12-01\n    acc = acc + spot()\n"
            "2025-03-01\n    acc = acc + spot()\n"
            "2025-06-01\n    acc = acc + spot()\n"
            "2025-09-01\n    acc = acc + spot()\n    pays max(acc / 5 - 100, 0)\n";

        ScriptedProduct<Real> script(src, ctx());
        const TimeLine &tl = script.timeline();
        ASSERT_EQ(tl.size(), 5u);

        SimulatableAsian<Real> ref(std::vector<Time>(tl.begin(), tl.end()), K,
                                   /*is_call=*/true, /*geometric=*/false);

        BlackScholesSimModel<Real> m1(S0, r, q, v), m2(S0, r, q, v);
        const auto a = simulate<Real>(script, m1, mc_settings());
        const auto b = simulate<Real>(ref, m2, mc_settings());

        // identical model, settings and timeline -> identical paths and payoffs
        EXPECT_NEAR(a.npv(), b.npv(), 1e-9 + 1e-10 * std::abs(b.npv()));
        EXPECT_NEAR(a.std_error(), b.std_error(), 1e-12);
    }

    // ── 3. scripted autocall vs BSAutocallMCEngine, independent MC ──────────
    //
    // The legacy engine overwrites the path PV on an autocall (`path_pv =`,
    // not `+=`), discarding any coupons paid at earlier dates. A script cannot
    // un-pay, so the two agree only where that reset is not exercised: a note
    // that (almost) never autocalls, and one that always autocalls at the
    // first date with no prior coupon. Together they cover coupon accrual with
    // memory, the missed-coupon counter, terminal knock-in and the autocall
    // redemption.

    namespace
    {
        std::string autocall_obs(double ac, double cp)
        {
            const std::string A = std::to_string(ac);
            const std::string C = std::to_string(cp);
            return "    if spot() >= " + A + " then\n"
                                             "        pays 1000 * (1 + 0.05 * (miss + 1))\n"
                                             "        alive = 0\n"
                                             "    else\n"
                                             "        if spot() >= " +
                   C + " then\n"
                       "            pays 1000 * 0.05 * (miss + 1)\n"
                       "            miss = 0\n"
                       "        else\n"
                       "            miss = miss + 1\n"
                       "        endIf\n"
                       "    endIf\n";
        }

        double legacy_autocall(const TimeLine &obs_times, double S0, double r,
                               double q, double v, double ac_frac,
                               double cp_frac, double put_frac, unsigned seed)
        {
            auto model = std::make_shared<BlackScholesModel>(S0, r, q, v);
            PricingSettings s = mc_settings();
            s.mc_seed = seed;
            PricingContext pctx{MarketView{}, s, model};
            BSAutocallMCEngine engine(pctx);
            AutocallNote note(
                std::vector<Time>(obs_times.begin(), obs_times.end()), ac_frac,
                cp_frac, put_frac, /*coupon_rate=*/0.05, /*notional=*/1000.0,
                /*memory=*/true, /*ki_continuous=*/false);
            return price(note, engine).npv;
        }
    } // namespace

    TEST(ScriptParity, AutocallCouponAndKnockInMatchLegacy)
    {
        const double S0 = 100, r = 0.04, q = 0.0, v = 0.22;
        const std::string obs = autocall_obs(1000.0, 70.0); // ac 10x S0: never called

        const std::string src =
            "2024-12-03\n    miss = 0\n    alive = 1\n" + obs +
            "2025-06-03\n    if alive = 1 then\n" + obs + "    endIf\n" +
            "2025-12-03\n    if alive = 1 then\n" + obs +
            "        if alive = 1 then\n"
            "            if spot() < 60 then pays 1000 * spot() / 100\n"
            "            else pays 1000 endIf\n"
            "        endIf\n"
            "    endIf\n";

        ScriptedProduct<Real> script(src, ctx());
        const TimeLine &tl = script.timeline();
        ASSERT_EQ(tl.size(), 3u);

        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto scripted = simulate<Real>(script, model, mc_settings());
        const double legacy =
            legacy_autocall(tl, S0, r, q, v, 10.0, 0.70, 0.60, 777);

        EXPECT_NEAR(scripted.npv(), legacy, 4.0 * scripted.std_error() + 0.5);
    }

    TEST(ScriptParity, AutocallRedemptionMatchesLegacy)
    {
        const double S0 = 100, r = 0.04, q = 0.0, v = 0.08;
        const std::string obs = autocall_obs(50.0, 50.0); // ac 0.5x S0: called at date 1

        const std::string src =
            "2024-12-03\n    miss = 0\n    alive = 1\n" + obs +
            "2025-06-03\n    if alive = 1 then\n" + obs + "    endIf\n" +
            "2025-12-03\n    if alive = 1 then\n" + obs +
            "        if alive = 1 then\n"
            "            if spot() < 30 then pays 1000 * spot() / 100\n"
            "            else pays 1000 endIf\n"
            "        endIf\n"
            "    endIf\n";

        ScriptedProduct<Real> script(src, ctx());
        const TimeLine &tl = script.timeline();

        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto scripted = simulate<Real>(script, model, mc_settings());
        const double legacy =
            legacy_autocall(tl, S0, r, q, v, 0.50, 0.50, 0.30, 777);

        // called at date 1 for every path: pays 1000 * 1.05 * df(t1)
        EXPECT_NEAR(scripted.npv(), legacy, 4.0 * scripted.std_error() + 0.5);
        EXPECT_NEAR(scripted.npv(), 1000.0 * 1.05 * std::exp(-r * tl.front()),
                    1.0);
    }

    // ── df(DATE): future discount factor lookup (WP 16e) ────────────────────

    TEST(ScriptParity, DfMatchesFlatRateDiscountFactor)
    {
        // A fixed cash flow, paid via df() to a maturity strictly after the
        // event's own date -- no spot() at all, so every path gives exactly
        // the same value: an exact-precision oracle, not an MC-noise-bounded
        // one. NodePays deflates by that EVENT's own numeraire, and df()
        // then re-discounts from there to the later maturity -- the two
        // exponentials collapse to plain notional * exp(-r * T_maturity),
        // independent of which event date the flow happens to be observed
        // from.
        const double S0 = 100, r = 0.03, q = 0.0, v = 0.2;
        ScriptedProduct<Real> prod(
            "2025-06-03\n    pays 1000 * df(2026-06-03)\n", ctx());
        BlackScholesSimModel<Real> model(S0, r, q, v);

        PricingSettings s;
        s.mc_paths = 100;
        s.mc_seed = 1;
        const auto res = simulate<Real>(prod, model, s);

        const Time T_mat = ctx().t(Date::from_iso("2026-06-03"));
        const double expected = 1000.0 * std::exp(-r * T_mat);
        EXPECT_NEAR(res.npv(), expected, 1e-6);
        EXPECT_NEAR(res.std_error(), 0.0, 1e-12);
    }

    TEST(ScriptParity, DfDiscountsADeferredSettlementPayoff)
    {
        // A European call observed at T_obs but cash-settled a year later,
        // at T_settle: PV = (ordinary BS call PV observed at T_obs) times
        // the extra period's discount factor -- df() is deterministic given
        // a flat r, so it factors straight out of the expectation.
        const double S0 = 100, K = 105, r = 0.03, q = 0.01, v = 0.2;
        ScriptedProduct<Real> prod(
            "2025-06-03\n    pays max(spot() - 105, 0) * df(2026-06-03)\n",
            ctx());
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());

        const Time T_obs = prod.timeline().front();
        const Time T_settle = ctx().t(Date::from_iso("2026-06-03"));
        const double extra_df = std::exp(-r * (T_settle - T_obs));
        const double expected = bs_call(S0, K, r, q, v, T_obs) * extra_df;
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
    }

    // ── historical fixings: an event on or before the valuation date is
    // replayed once, against a caller-supplied fixing, and only its
    // variable state carries into the future (WP 16e) ───────────────────────

    TEST(ScriptParity, HistoricalFixingSeedsAVariableTheFuturePayoffReads)
    {
        // A call whose strike was fixed before today, at a historical spot
        // of 90 -- struck below today's S0 = 100, so this is exactly
        // bs_call(S0, K=90, ...) with K sourced from a fixing instead of a
        // script literal.
        const double S0 = 100, r = 0.03, q = 0.01, v = 0.2;
        const std::string src = "2024-01-03\n    k = spot()\n"
                                "2025-06-03\n    pays max(spot() - k, 0)\n";
        const std::map<Date, std::vector<double>> fixings{
            {Date::from_iso("2024-01-03"), {90.0}}};

        ScriptedProduct<Real> prod(src, ctx(), {}, fixings);
        ASSERT_EQ(prod.timeline().size(), 1u); // the historical event isn't simulated

        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, mc_settings());

        const double T = prod.timeline().front();
        EXPECT_NEAR(res.npv(), bs_call(S0, 90.0, r, q, v, T),
                    4.0 * res.std_error());
    }

    TEST(ScriptParity, MissingHistoricalFixingThrows)
    {
        const std::string src =
            "2024-01-03\n    k = spot()\n2025-06-03\n    pays max(spot() - k, 0)\n";
        EXPECT_THROW((ScriptedProduct<Real>(src, ctx(), {}, {})), InvalidInput);
    }

    TEST(ScriptParity, DfInsideAHistoricalEventThrowsAtConstruction)
    {
        const std::string src =
            "2024-01-03\n    x = df(2025-01-01)\n2025-06-03\n    pays x\n";
        EXPECT_THROW((ScriptedProduct<Real>(src, ctx(), {}, {})), InvalidInput);
    }

    TEST(ScriptParity, HistoricalPaysIsASunkCostNotCountedToday)
    {
        // The 500 paid before the valuation date already happened; only the
        // 1000 paid in the future is part of today's price.
        const std::string src = "2024-01-03\n    pays 500\n2025-06-03\n    pays 1000\n";
        const std::map<Date, std::vector<double>> fixings{
            {Date::from_iso("2024-01-03"), {}}};

        ScriptedProduct<Real> prod(src, ctx(), {}, fixings);
        BlackScholesSimModel<Real> model(100.0, 0.03, 0.0, 0.2);

        PricingSettings s;
        s.mc_paths = 100;
        s.mc_seed = 1;
        const auto res = simulate<Real>(prod, model, s);

        const double T = prod.timeline().front();
        EXPECT_NEAR(res.npv(), 1000.0 * std::exp(-0.03 * T), 1e-6);
    }

    // ── 16e's own acceptance criterion: a multi-underlying script priced
    // against BSMountainMCEngine (blueprint/wp/16-scripting.md §10) ─────────

    namespace
    {
        double legacy_mountain(double S0_0, double S0_1, double r, double q,
                               double v0, double v1, double rho, Time T1,
                               Time T2, unsigned seed)
        {
            Eigen::MatrixXd corr(2, 2);
            corr << 1.0, rho, rho, 1.0;
            auto model = std::make_shared<MultiAssetBSModel>(
                r, std::vector<Real>{S0_0, S0_1}, std::vector<Real>{v0, v1},
                std::vector<Real>{q, q}, corr);
            PricingSettings s = mc_settings();
            s.mc_seed = seed;
            PricingContext pctx{MarketView{}, s, model};
            BSMountainMCEngine engine(pctx);
            MountainOption opt({T1, T2}, /*strike=*/0.0, /*is_call=*/true,
                               /*notional=*/100.0);
            return price(opt, engine).npv;
        }
    } // namespace

    TEST(ScriptParity, TwoAssetHimalayaMatchesBSMountainMCEngine)
    {
        // Hand-unrolled Himalaya for n = 2 (the language has no loops, so a
        // fixed n is written out explicitly -- WP §11): at T1, lock in the
        // better of the two returns and remove that asset; at T2 (maturity),
        // the *other* asset's return is measured at T2, not T1 -- it stayed
        // in the basket. avg of the two locked-in returns, call struck at 0.
        const double S0_0 = 100, S0_1 = 100, r = 0.03, q = 0.0, v0 = 0.20,
                    v1 = 0.30, rho = 0.3;
        const std::string src =
            "2024-12-03\n"
            "    r0 = spot(0) / 100 - 1\n"
            "    r1 = spot(1) / 100 - 1\n"
            "    if r0 >= r1 then\n"
            "        perf1 = r0\n"
            "        winner0 = 1\n"
            "    else\n"
            "        perf1 = r1\n"
            "        winner0 = 0\n"
            "    endIf\n"
            "2025-06-03\n"
            "    if winner0 = 1 then\n"
            "        perf2 = spot(1) / 100 - 1\n"
            "    else\n"
            "        perf2 = spot(0) / 100 - 1\n"
            "    endIf\n"
            "    avg = (perf1 + perf2) / 2\n"
            "    pays 100 * max(avg, 0)\n";

        ScriptedProduct<Real> script(src, ctx());
        ASSERT_EQ(script.n_underlyings(), 2u);
        const TimeLine &tl = script.timeline();
        ASSERT_EQ(tl.size(), 2u);

        Eigen::MatrixXd corr(2, 2);
        corr << 1.0, rho, rho, 1.0;
        MultiAssetBSSimModel<Real> model(
            std::vector<Real>{S0_0, S0_1}, r, std::vector<Real>{q, q},
            std::vector<Real>{v0, v1}, corr);
        const auto scripted = simulate<Real>(script, model, mc_settings());

        const double legacy = legacy_mountain(S0_0, S0_1, r, q, v0, v1, rho,
                                              tl[0], tl[1], 777);

        EXPECT_NEAR(scripted.npv(), legacy, 4.0 * scripted.std_error() + 0.5);
    }

} // namespace quantModeling
