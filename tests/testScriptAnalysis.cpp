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
        using scripting::advise;
        using scripting::Advice;
        using scripting::ModelKind;
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

        const std::string vanilla = "2025-06-03\n    pays max(spot() - 100, 0)\n";
        const std::string linear_forward = "2025-06-03\n    pays spot() - 100\n";
        const std::string digital =
            "2025-06-03\n    if spot() > 100 then pays 1 endIf\n";
        const std::string knock_in =
            "2024-12-03\n    if spot() < 70 then ki = 1 endIf\n"
            "2025-06-03\n"
            "    if ki = 1 and spot() < 100 then pays spot()\n"
            "    else pays 100 endIf\n";
        const std::string asian =
            "2024-12-03\n    acc = spot()\n"
            "2025-06-03\n    acc = acc + spot()\n    pays max(acc / 2 - 100, 0)\n";
        const std::string forward_start =
            "2024-12-03\n    k = spot()\n"
            "2025-06-03\n    pays max(spot() - k, 0)\n";
        const std::string no_spot_at_all =
            "2024-12-03\n    c = 0\n"
            "2025-06-03\n    c = c + 25\n    pays 1000 + c\n";
    } // namespace

    // ── what the script's price depends on ──────────────────────────────────

    TEST(ScriptAnalysis, VanillaIsNonlinearButNeitherThresholdNorPathDependent)
    {
        const ScriptAnalysis a = analyze(vanilla);
        EXPECT_TRUE(a.nonlinear_in_spot);
        EXPECT_FALSE(a.spot_threshold_test);
        EXPECT_FALSE(a.path_dependent);
    }

    TEST(ScriptAnalysis, ALinearPayoffDependsOnNoSmileAtAll)
    {
        const ScriptAnalysis a = analyze(linear_forward);
        EXPECT_FALSE(a.nonlinear_in_spot);
        EXPECT_FALSE(a.spot_threshold_test);
        EXPECT_FALSE(a.path_dependent);
    }

    TEST(ScriptAnalysis, ADigitalIsAThresholdTest)
    {
        const ScriptAnalysis a = analyze(digital);
        EXPECT_TRUE(a.nonlinear_in_spot);
        EXPECT_TRUE(a.spot_threshold_test);
        EXPECT_FALSE(a.path_dependent);
    }

    TEST(ScriptAnalysis, AKnockInFlagCarriesThePathEvenThoughItIsAssignedAConstant)
    {
        // `ki = 1` never mentions spot, but it is assigned under a
        // spot-derived condition: that is exactly how the flag records the
        // path, so it must count as spot-derived and carried across events.
        const ScriptAnalysis a = analyze(knock_in);
        EXPECT_TRUE(a.spot_threshold_test);
        EXPECT_TRUE(a.path_dependent);
    }

    TEST(ScriptAnalysis, ARunningSumIsPathDependent)
    {
        const ScriptAnalysis a = analyze(asian);
        EXPECT_TRUE(a.path_dependent);
        EXPECT_TRUE(a.nonlinear_in_spot);
    }

    TEST(ScriptAnalysis, AReferenceLevelFixedEarlierIsPathDependent)
    {
        EXPECT_TRUE(analyze(forward_start).path_dependent);
    }

    TEST(ScriptAnalysis, QuantitiesNotDerivedFromSpotAreNotPathDependence)
    {
        // A coupon counter carried across events, no spot anywhere.
        const ScriptAnalysis a = analyze(no_spot_at_all);
        EXPECT_FALSE(a.path_dependent);
        EXPECT_FALSE(a.nonlinear_in_spot);
    }

    TEST(ScriptAnalysis, ReadingAVariableInTheEventThatAssignsItIsNotPathDependence)
    {
        const ScriptAnalysis a = analyze(
            "2025-06-03\n    x = spot()\n    pays max(x - 100, 0)\n");
        EXPECT_FALSE(a.path_dependent);
        EXPECT_TRUE(a.nonlinear_in_spot);
    }

    TEST(ScriptAnalysis, ReportsTheNumberOfUnderlyingsTheScriptReads)
    {
        EXPECT_EQ(analyze(vanilla).n_underlyings, 1u);
        EXPECT_EQ(analyze("2025-06-03\n    pays max(spot(1) - 100, 0)\n")
                      .n_underlyings,
                  2u);
    }

    // ── what to tell the user, given the model they chose ───────────────────

    TEST(ModelAdvice, FlatVolWarnsAboutAThresholdTestAndNamesTheFix)
    {
        const auto adv =
            advise(analyze(digital), ModelKind::BlackScholesFlatVol, 1.0, 0.0);
        ASSERT_TRUE(has(adv, "flat_vol_smile"));
        const auto it = std::find_if(adv.begin(), adv.end(), [](const Advice &a)
                                     { return a.code == "flat_vol_smile"; });
        EXPECT_EQ(it->severity, "warning");
        EXPECT_NE(it->message.find("local_vol"), std::string::npos);
        EXPECT_NE(it->message.find("skew"), std::string::npos);
    }

    TEST(ModelAdvice, FlatVolAlsoWarnsForAPlainNonlinearPayoff)
    {
        EXPECT_TRUE(has(advise(analyze(vanilla), ModelKind::BlackScholesFlatVol,
                               1.0, 0.0),
                        "flat_vol_smile"));
    }

    TEST(ModelAdvice, FlatVolIsFineForALinearPayoff)
    {
        EXPECT_TRUE(advise(analyze(linear_forward),
                           ModelKind::BlackScholesFlatVol, 1.0, 0.0)
                        .empty());
    }

    TEST(ModelAdvice, ThePathDependenceNoteAppliesToBothModels)
    {
        for (ModelKind m : {ModelKind::BlackScholesFlatVol,
                            ModelKind::LocalVolSurface})
        {
            const auto adv = advise(analyze(knock_in), m, 1.0, 2.0);
            ASSERT_TRUE(has(adv, "forward_smile"));
            const auto it = std::find_if(adv.begin(), adv.end(),
                                         [](const Advice &a)
                                         { return a.code == "forward_smile"; });
            EXPECT_EQ(it->severity, "info");
        }
    }

    TEST(ModelAdvice, LocalVolDoesNotGetTheFlatVolWarning)
    {
        EXPECT_FALSE(has(advise(analyze(digital), ModelKind::LocalVolSurface,
                                1.0, 2.0),
                         "flat_vol_smile"));
    }

    TEST(ModelAdvice, LocalVolWarnsWhenEventsOutrunTheCalibratedSurface)
    {
        const auto adv =
            advise(analyze(vanilla), ModelKind::LocalVolSurface, 2.5, 1.2);
        ASSERT_TRUE(has(adv, "surface_extrapolated"));
        const auto it = std::find_if(adv.begin(), adv.end(), [](const Advice &a)
                                     { return a.code == "surface_extrapolated"; });
        EXPECT_NE(it->message.find("2.50"), std::string::npos);
        EXPECT_NE(it->message.find("1.20"), std::string::npos);
    }

    TEST(ModelAdvice, NoExtrapolationWarningInsideTheSurface)
    {
        EXPECT_FALSE(has(advise(analyze(vanilla), ModelKind::LocalVolSurface,
                                1.0, 1.2),
                         "surface_extrapolated"));
    }

} // namespace quantModeling
