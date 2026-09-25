#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/model_advice.hpp"

#include <algorithm>
#include <string>

namespace quantModeling
{
    namespace
    {
        using scripting::Advice;
        using scripting::advise;
        using scripting::ModelAvailability;
        using scripting::ModelKind;
        using scripting::recommend;
        using scripting::ScriptAnalysis;

        ScriptAnalysis analyze(const std::string &src)
        {
            const ValuationContext ctx{Date::from_iso("2024-06-03")};
            return ScriptedProduct<Real>(src, ctx).analysis();
        }

        bool has(const std::vector<Advice> &v, const std::string &code)
        {
            return std::any_of(v.begin(), v.end(),
                               [&](const Advice &a)
                               { return a.code == code; });
        }

        constexpr ModelAvailability kEverything{true, true};

        const std::string vanilla = "2025-06-03\n    pays max(spot() - 100, 0)\n";
        const std::string linear_forward = "2025-06-03\n    pays spot() - 100\n";
        const std::string digital =
            "2025-06-03\n    if spot() > 100 then pays 1 endIf\n";
        const std::string two_europeans =
            "2024-12-03\n    pays max(spot() - 100, 0)\n"
            "2025-06-03\n    pays max(100 - spot(), 0)\n";
        const std::string down_and_out =
            "2024-12-03\n    alive = 1\n    if spot() < 80 then alive = 0 endIf\n"
            "2025-06-03\n    if spot() < 80 then alive = 0 endIf\n"
            "    if alive = 1 then pays max(spot() - 100, 0) endIf\n";
        /// The structured product of the question: a vanilla and a barrier
        /// in one script. One model for the whole of it.
        const std::string vanilla_plus_barrier =
            "2024-12-03\n    alive = 1\n    if spot() < 80 then alive = 0 endIf\n"
            "2025-06-03\n    if spot() < 80 then alive = 0 endIf\n"
            "    pays max(spot() - 100, 0)\n"
            "    if alive = 1 then pays max(95 - spot(), 0) endIf\n";
    } // namespace

    TEST(ModelRecommendation, AVanillaNeedsOnlyTodaysSurface)
    {
        const auto r = recommend(analyze(vanilla), kEverything);
        EXPECT_EQ(r.model, ModelKind::LocalVolSurface);
        EXPECT_EQ(r.code, "terminal_smile");
    }

    TEST(ModelRecommendation, SeveralEuropeanDatesStillDependOnlyOnMarginals)
    {
        EXPECT_EQ(recommend(analyze(two_europeans), kEverything).model,
                  ModelKind::LocalVolSurface);
        EXPECT_EQ(recommend(analyze(digital), kEverything).model,
                  ModelKind::LocalVolSurface);
    }

    TEST(ModelRecommendation, ABarrierNeedsStochasticLocalVol)
    {
        const auto r = recommend(analyze(down_and_out), kEverything);
        EXPECT_EQ(r.model, ModelKind::StochasticLocalVol);
        EXPECT_EQ(r.code, "path_dependent");
    }

    TEST(ModelRecommendation, AVanillaPlusABarrierGetsTheBarriersModelForTheWhole)
    {
        EXPECT_EQ(recommend(analyze(vanilla_plus_barrier), kEverything).model,
                  ModelKind::StochasticLocalVol);
    }

    TEST(ModelRecommendation, FallsBackToLocalVolWhenNoStochasticCalibration)
    {
        const auto r = recommend(analyze(down_and_out), ModelAvailability{true, false});
        EXPECT_EQ(r.model, ModelKind::LocalVolSurface);
        EXPECT_EQ(r.code, "stochastic_unavailable");
    }

    TEST(ModelRecommendation, WithoutMarketDataOnlyFlatVolIsLeft)
    {
        for (const std::string *s : {&vanilla, &down_and_out, &linear_forward})
        {
            const auto r = recommend(analyze(*s), ModelAvailability{false, false});
            EXPECT_EQ(r.model, ModelKind::BlackScholesFlatVol);
            EXPECT_EQ(r.code, "no_market_data");
        }
    }

    TEST(ModelRecommendation, HestonAloneIsNeverRecommended)
    {
        for (const std::string *s : {&vanilla, &linear_forward, &digital, &down_and_out,
                                     &vanilla_plus_barrier})
            for (const ModelAvailability avail :
                 {ModelAvailability{true, true}, ModelAvailability{true, false},
                  ModelAvailability{false, false}})
                EXPECT_NE(recommend(analyze(*s), avail).model, ModelKind::Heston);
    }

    TEST(ModelRecommendation, TheRecommendedModelNeverDrawsAWarning)
    {
        // Whatever advise() would warn about, recommend() has already
        // avoided when the market allows: only info notes may remain.
        for (const std::string *s : {&vanilla, &linear_forward, &digital, &two_europeans,
                                     &down_and_out, &vanilla_plus_barrier})
        {
            const auto r = recommend(analyze(*s), kEverything);
            for (const Advice &a : advise(analyze(*s), r.model, 1.0, 2.0))
                EXPECT_NE(a.severity, "warning") << a.code << " on " << *s;
        }
    }

    TEST(ModelAdvice, HestonNotesItsFitErrorOnASmileSensitivePayoff)
    {
        EXPECT_TRUE(has(advise(analyze(vanilla), ModelKind::Heston, 1.0, 0.0), "heston_fit"));
        EXPECT_FALSE(has(advise(analyze(linear_forward), ModelKind::Heston, 1.0, 0.0),
                         "heston_fit"));
    }

    TEST(ModelAdvice, SLVStatesItsForwardSmileAssumptionAndItsSurfaceHorizon)
    {
        const auto adv = advise(analyze(down_and_out), ModelKind::StochasticLocalVol, 2.5, 1.2);
        EXPECT_TRUE(has(adv, "forward_smile"));
        EXPECT_TRUE(has(adv, "surface_extrapolated"));
        EXPECT_FALSE(has(adv, "flat_vol_smile"));
    }

    TEST(ModelAdvice, ModelNamesMatchWhatPriceScriptAccepts)
    {
        EXPECT_STREQ(scripting::model_name(ModelKind::BlackScholesFlatVol), "black_scholes");
        EXPECT_STREQ(scripting::model_name(ModelKind::LocalVolSurface), "local_vol");
        EXPECT_STREQ(scripting::model_name(ModelKind::Heston), "heston");
        EXPECT_STREQ(scripting::model_name(ModelKind::StochasticLocalVol), "slv");
    }

    TEST(ModelRecommendation, SeveralUnderlyingsGetCorrelatedBlackScholesWithTheCorrelationNoted)
    {
        const std::string worst_of =
            "2025-06-03\n    pays max(min(spot(0) / 100, spot(1) / 50) - 1, 0)\n";
        for (const ModelAvailability avail : {ModelAvailability{true, true},
                                              ModelAvailability{false, false}})
        {
            const auto r = recommend(analyze(worst_of), avail);
            EXPECT_EQ(r.model, ModelKind::BlackScholesFlatVol);
            EXPECT_EQ(r.code, "multi_asset");
        }
        const auto adv = advise(analyze(worst_of), ModelKind::BlackScholesFlatVol, 1.0, 0.0);
        EXPECT_TRUE(has(adv, "correlation"));
        EXPECT_TRUE(has(adv, "flat_vol_smile"));
    }

} // namespace quantModeling
