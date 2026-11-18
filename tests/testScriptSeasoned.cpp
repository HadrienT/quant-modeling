#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"

#include <map>
#include <string>
#include <vector>

// A seasoned product held in a portfolio (issue 313, lot b3): the cash flows
// its past events paid are reported (ScriptedProduct::historical_payments),
// and a past exercise date is a right the holder did not use when
// ScriptSettings::past_rights_unexercised is set -- refused otherwise, as
// before.

namespace quantModeling
{
    namespace
    {
        const Date kToday = Date::from_iso("2024-06-03");
        ValuationContext ctx()
        {
            return ValuationContext{kToday};
        }
        std::map<Date, std::vector<double>> fixings(std::initializer_list<std::pair<const char *, double>> f)
        {
            std::map<Date, std::vector<double>> out;
            for (const auto &[iso, s] : f)
                out[Date::from_iso(iso)] = {s};
            return out;
        }

        /// Three coupons of 5 % of the initial level, then the stock.
        const char *kCoupons = "2024-03-01\n    s0 = spot()\n    pays 0.05 * s0\n"
                               "2024-05-01\n    pays 0.05 * s0\n"
                               "2024-09-02\n    pays 0.05 * s0\n"
                               "2025-03-03\n    pays spot()\n";

        /// A Bermudan put on the initial level, exercisable on three dates.
        const char *kBermudan = "2024-03-01\n    s0 = spot()\n"
                                "2024-05-01\n    if spot() < s0 and exercise(spot()) then\n        pays s0 - spot()\n"
                                "    endIf\n"
                                "2024-09-02\n    if spot() < s0 and exercise(spot()) then\n        pays s0 - spot()\n"
                                "    endIf\n"
                                "2025-03-03\n    if spot() < s0 and exercise(spot()) then\n        pays s0 - spot()\n"
                                "    endIf\n";
    } // namespace

    TEST(ScriptSeasoned, ThePastCouponsAreReportedAndLeftOutOfThePrice)
    {
        const auto f = fixings({{"2024-03-01", 100.0}, {"2024-05-01", 104.0}});
        ScriptedProduct<Real> product(kCoupons, ctx(), {}, f);
        // two coupons of 5 on s0 = 100, undiscounted
        EXPECT_DOUBLE_EQ(product.historical_payments(), 10.0);
        // without past events, nothing was paid
        ScriptedProduct<Real> fresh("2025-03-03\n    pays spot()\n", ctx());
        EXPECT_EQ(fresh.historical_payments(), 0.0);
    }

    TEST(ScriptSeasoned, APastExerciseDateIsRefusedUnlessTheRightWasNotUsed)
    {
        const auto f = fixings({{"2024-03-01", 100.0}, {"2024-05-01", 90.0}});
        EXPECT_THROW(ScriptedProduct<Real>(kBermudan, ctx(), {}, f), InvalidInput);

        ScriptSettings s;
        s.past_rights_unexercised = true;
        ScriptedProduct<Real> seasoned(kBermudan, ctx(), s, f);
        // the past exercise date paid nothing: it was skipped, not taken
        EXPECT_EQ(seasoned.historical_payments(), 0.0);
        EXPECT_EQ(seasoned.timeline().size(), 2u);

        // what is left is the same right on the two future dates, struck at
        // the s0 the replay fixed
        const char *rest = "2024-09-02\n    if spot() < 100 and exercise(spot()) then\n        pays 100 - spot()\n"
                           "    endIf\n"
                           "2025-03-03\n    if spot() < 100 and exercise(spot()) then\n        pays 100 - spot()\n"
                           "    endIf\n";
        ScriptedProduct<Real> fresh(rest, ctx());
        PricingSettings ps;
        ps.mc_paths = 50000;
        ps.mc_seed = 3;
        BlackScholesSimModel<Real> m1(95.0, 0.03, 0.0, 0.25), m2(95.0, 0.03, 0.0, 0.25);
        fit_exercise_policy(seasoned, m1, 50000, 3);
        fit_exercise_policy(fresh, m2, 50000, 3);
        const auto a = simulate<Real>(seasoned, m1, ps), b = simulate<Real>(fresh, m2, ps);
        EXPECT_NEAR(a.npv(), b.npv(), 1e-12 * b.npv());
    }

} // namespace quantModeling
