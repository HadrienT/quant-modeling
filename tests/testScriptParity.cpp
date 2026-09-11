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

#include <cmath>
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

} // namespace quantModeling
