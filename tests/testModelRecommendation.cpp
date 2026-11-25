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

    // Several underlyings: each on its own Dupire surface when every one has
    // a stored chain (issue #86), correlated Black-Scholes otherwise; the
    // correlation is an input either way.
    TEST(ModelRecommendation, SeveralUnderlyingsGetLocalVolWhenEveryChainIsStored)
    {
        const std::string worst_of =
            "2025-06-03\n    pays max(min(spot(0) / 100, spot(1) / 50) - 1, 0)\n";
        const auto lv = recommend(analyze(worst_of), ModelAvailability{true, false});
        EXPECT_EQ(lv.model, ModelKind::LocalVolSurface);
        EXPECT_EQ(lv.code, "multi_asset_local_vol");
        const auto bs = recommend(analyze(worst_of), ModelAvailability{false, false});
        EXPECT_EQ(bs.model, ModelKind::BlackScholesFlatVol);
        EXPECT_EQ(bs.code, "multi_asset");

        const auto flat = advise(analyze(worst_of), ModelKind::BlackScholesFlatVol, 1.0, 0.0);
        EXPECT_TRUE(has(flat, "correlation"));
        EXPECT_TRUE(has(flat, "flat_vol_smile"));
        const auto local = advise(analyze(worst_of), ModelKind::LocalVolSurface, 1.0, 2.0);
        EXPECT_TRUE(has(local, "correlation"));
        EXPECT_FALSE(has(local, "flat_vol_smile"));
    }

    // The pathwise risks to the variance parameters exist only while the
    // Feller ratio is at least 1 (model_advice.hpp): below it they are
    // removed with a reason, the spot, rate, dividend and leverage risks kept.
    TEST(ModelAdvice, VarianceParameterRisksAreWithheldBelowFeller)
    {
        const auto report = []
        {
            return RiskReport{{"spot", "rate", "div", "v0", "kappa", "theta", "xi", "rho", "leverage[0,0]"},
                              {0.6, 200.0, -240.0, -5e8, -1e6, -1e8, 6e7, -1e8, 3.0},
                              {0.01, 2.0, 2.0, 6e8, 4e6, 2e8, 7e7, 1e8, 0.1}};
        };
        const HestonParams below{0.0755, 3.09, 0.0886, 1.755, -0.36}; // ratio 0.18 (AAPL, 2026-11-17)
        const HestonParams above{0.04, 1.5, 0.04, 0.3, -0.6};         // ratio 1.33

        RiskReport slv = report();
        const auto why = scripting::withhold_variance_parameter_risks(slv, below, ModelKind::StochasticLocalVol);
        ASSERT_TRUE(why.has_value());
        EXPECT_EQ(why->code, "variance_parameter_risks_withheld");
        EXPECT_NE(why->message.find("0.18"), std::string::npos);
        const std::vector<std::string> kept{"spot", "rate", "div", "leverage[0,0]"};
        EXPECT_EQ(slv.labels, kept);
        EXPECT_EQ(slv.values.size(), kept.size());
        EXPECT_DOUBLE_EQ(slv.values[3], 3.0);
        EXPECT_DOUBLE_EQ(slv.std_errors[3], 0.1);

        RiskReport heston = report();
        EXPECT_TRUE(scripting::withhold_variance_parameter_risks(heston, below, ModelKind::Heston));
        EXPECT_EQ(heston.labels.size(), 4u);

        RiskReport healthy = report();
        EXPECT_FALSE(scripting::withhold_variance_parameter_risks(healthy, above, ModelKind::StochasticLocalVol));
        EXPECT_EQ(healthy.labels.size(), 9u);

        // Rough Bergomi's rho is its own parameter, not Heston's.
        RiskReport other = report();
        EXPECT_FALSE(scripting::withhold_variance_parameter_risks(other, below, ModelKind::LocalVolSurface));
        EXPECT_EQ(other.labels.size(), 9u);
    }

    // lastStepDigital's pathwise greeks divide by the last step's volatility
    // (#372): under Heston or SLV below Feller they have infinite variance and
    // are all withheld; above it, under local vol, or for a script without
    // lastStepDigital (a call on lastStepCall) they stay.
    TEST(ModelAdvice, LastStepDigitalGreeksAreWithheldBelowFeller)
    {
        const auto report = []
        { return RiskReport{{"spot", "rate", "div"}, {0.39, 217.0, -280.0}, {0.23, 166.0, 170.0}}; };
        const HestonParams below{0.0755, 3.09, 0.0886, 1.755, -0.36}; // ratio 0.18
        const HestonParams above{0.04, 1.5, 0.04, 0.3, -0.6};         // ratio 1.33
        const auto digital = analyze("2025-06-03\n    pays 100 * lastStepDigital(100)\n");
        const auto call = analyze("2025-06-03\n    pays lastStepCall(100)\n");
        EXPECT_TRUE(digital.last_step_digital);
        EXPECT_FALSE(call.last_step_digital);

        for (const ModelKind m : {ModelKind::Heston, ModelKind::StochasticLocalVol})
        {
            RiskReport r = report();
            const auto why = scripting::withhold_last_step_digital_greeks(r, digital, below, m);
            ASSERT_TRUE(why.has_value());
            EXPECT_EQ(why->code, "last_step_digital_greeks_withheld");
            EXPECT_EQ(why->severity, "warning");
            EXPECT_NE(why->message.find("0.18"), std::string::npos);
            EXPECT_TRUE(r.labels.empty() && r.values.empty() && r.std_errors.empty());
        }

        RiskReport healthy = report();
        EXPECT_FALSE(scripting::withhold_last_step_digital_greeks(healthy, digital, above, ModelKind::Heston));
        RiskReport local = report();
        EXPECT_FALSE(
            scripting::withhold_last_step_digital_greeks(local, digital, below, ModelKind::LocalVolSurface));
        RiskReport smooth = report();
        EXPECT_FALSE(scripting::withhold_last_step_digital_greeks(smooth, call, below, ModelKind::Heston));
        for (const RiskReport *r : {&healthy, &local, &smooth})
            EXPECT_EQ(r->labels.size(), 3u);
    }

} // namespace quantModeling
