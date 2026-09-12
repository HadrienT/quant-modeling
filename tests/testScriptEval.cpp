#include <gtest/gtest.h>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/scripting/evaluator.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/const_cond.hpp"
#include "quantModeling/scripting/visitors/if_processor.hpp"
#include "quantModeling/scripting/visitors/var_indexer.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace quantModeling::scripting
{
    namespace
    {
        struct EvalResult
        {
            double payoff = 0.0;
            std::vector<double> vars;
            std::vector<std::string> names;

            double var(const std::string &name) const
            {
                for (std::size_t i = 0; i < names.size(); ++i)
                    if (names[i] == name)
                        return vars[i];
                ADD_FAILURE() << "no such variable: " << name;
                return 0.0;
            }
        };

        /// Drive the hard Evaluator directly: one Sample per event, spots and
        /// numeraires supplied. Events are sorted by date, so pass spots in the
        /// same date order.
        EvalResult run_hard(const std::string &source,
                            const std::vector<double> &spots,
                            const std::vector<double> &numeraires = {})
        {
            std::vector<Event> events = parse_script(source);
            std::stable_sort(events.begin(), events.end(),
                             [](const Event &a, const Event &b)
                             { return a.date < b.date; });

            VarIndexer indexer;
            indexer.index(events);
            ConstCondProcessor().process(events);
            IfProcessor().process(events);

            EXPECT_EQ(events.size(), spots.size());

            Scenario<Real> scenario(events.size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                scenario[i].spots.assign(1, spots[i]);
                scenario[i].numeraire =
                    numeraires.empty() ? 1.0 : numeraires[i];
            }

            Evaluator<Real> ev;
            ev.set_variable_count(indexer.count());
            ev.initialize();
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                ev.set_event(scenario, i);
                for (const ExprTree &stmt : events[i].statements)
                    ev.run(*stmt);
            }

            EvalResult r;
            r.payoff = static_cast<double>(ev.payoff());
            r.names = indexer.names();
            for (const Real &v : ev.variables())
                r.vars.push_back(static_cast<double>(v));
            return r;
        }

        /// Like run_hard, but each event supplies spots for several
        /// underlyings (asset-major: spots[event * n_assets + asset]) --
        /// for spot(i), blueprint/wp/17-aad.md lot 17e.
        EvalResult run_hard_multi(const std::string &source, std::size_t n_assets,
                                  const std::vector<double> &spots)
        {
            std::vector<Event> events = parse_script(source);
            std::stable_sort(events.begin(), events.end(),
                             [](const Event &a, const Event &b)
                             { return a.date < b.date; });

            VarIndexer indexer;
            indexer.index(events);
            ConstCondProcessor().process(events);
            IfProcessor().process(events);

            EXPECT_EQ(events.size() * n_assets, spots.size());

            Scenario<Real> scenario(events.size());
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                scenario[i].spots.assign(spots.begin() + static_cast<std::ptrdiff_t>(i * n_assets),
                                         spots.begin() + static_cast<std::ptrdiff_t>((i + 1) * n_assets));
                scenario[i].numeraire = 1.0;
            }

            Evaluator<Real> ev;
            ev.set_variable_count(indexer.count());
            ev.initialize();
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                ev.set_event(scenario, i);
                for (const ExprTree &stmt : events[i].statements)
                    ev.run(*stmt);
            }

            EvalResult r;
            r.payoff = static_cast<double>(ev.payoff());
            r.names = indexer.names();
            for (const Real &v : ev.variables())
                r.vars.push_back(static_cast<double>(v));
            return r;
        }
    } // namespace

    TEST(ScriptEval, ArithmeticAndFunctions)
    {
        auto r = run_hard("2025-01-01\n    x = 2 + 3 * 4\n    y = 2 ^ 3 ^ 2\n"
                          "    z = min(max(spot(), 5), 100)\n"
                          "    w = sqrt(abs(-16)) + log(exp(1))\n",
                          {40.0});
        EXPECT_DOUBLE_EQ(r.var("x"), 14.0);
        EXPECT_DOUBLE_EQ(r.var("y"), 512.0); // right assoc: 2^(3^2)
        EXPECT_DOUBLE_EQ(r.var("z"), 40.0);
        EXPECT_DOUBLE_EQ(r.var("w"), 5.0);
    }

    TEST(ScriptEval, VariablesPersistAcrossEvents)
    {
        auto r = run_hard("2025-01-01\n    acc = spot()\n"
                          "2025-02-01\n    acc = acc + spot()\n"
                          "2025-03-01\n    acc = acc + spot()\n"
                          "    pays acc / 3\n",
                          {90.0, 100.0, 110.0});
        EXPECT_DOUBLE_EQ(r.var("acc"), 300.0);
        EXPECT_DOUBLE_EQ(r.payoff, 100.0);
    }

    TEST(ScriptEval, UntakenBranchDoesNotAssign)
    {
        auto r = run_hard("2025-01-01\n    x = 1\n"
                          "    if spot() > 100 then x = 2 endIf\n"
                          "    if spot() < 100 then y = 7 else y = 9 endIf\n",
                          {50.0});
        EXPECT_DOUBLE_EQ(r.var("x"), 1.0); // condition false, no write
        EXPECT_DOUBLE_EQ(r.var("y"), 7.0); // then-branch taken
    }

    TEST(ScriptEval, BooleanConnectives)
    {
        auto in = run_hard("2025-01-01\n"
                           "    if spot() > 90 and spot() < 110 then hit = 1 "
                           "else hit = 0 endIf\n",
                           {100.0});
        EXPECT_DOUBLE_EQ(in.var("hit"), 1.0);
        auto out = run_hard("2025-01-01\n"
                            "    if spot() > 90 and spot() < 110 then hit = 1 "
                            "else hit = 0 endIf\n",
                            {130.0});
        EXPECT_DOUBLE_EQ(out.var("hit"), 0.0);
    }

    TEST(ScriptEval, PaysIsDeflatedByEventNumeraire)
    {
        auto r = run_hard("2025-01-01\n    pays 100\n"
                          "2025-07-01\n    pays 100\n",
                          {100.0, 100.0}, {1.25, 2.0});
        EXPECT_DOUBLE_EQ(r.payoff, 100.0 / 1.25 + 100.0 / 2.0);
    }

    TEST(ScriptEval, MemoryCouponAccrual)
    {
        // miss, miss, then a coupon date pays 3 periods and resets the counter
        auto r = run_hard(
            "2025-03-01\n    miss = 0\n"
            "    if spot() >= 100 then pays 5 * (miss + 1)\n        miss = 0\n"
            "    else miss = miss + 1 endIf\n"
            "2025-06-01\n"
            "    if spot() >= 100 then pays 5 * (miss + 1)\n        miss = 0\n"
            "    else miss = miss + 1 endIf\n"
            "2025-09-01\n"
            "    if spot() >= 100 then pays 5 * (miss + 1)\n        miss = 0\n"
            "    else miss = miss + 1 endIf\n",
            {80.0, 90.0, 105.0});
        EXPECT_DOUBLE_EQ(r.var("miss"), 0.0);
        EXPECT_DOUBLE_EQ(r.payoff, 15.0); // 5 * (2 + 1)
    }

    TEST(ScriptEval, ConstConditionIsFolded)
    {
        std::vector<Event> events = parse_script(
            "2025-01-01\n    if 1 > 0 then x = 1 endIf\n"
            "    if 2 < 1 then y = 1 endIf\n");
        VarIndexer().index(events);
        ConstCondProcessor().process(events);

        const Node &if_true =
            *events[0].statements[0]->arguments[0]; // Collect -> If
        const Node &if_false = *events[0].statements[1]->arguments[0];
        const auto &c_true =
            static_cast<const NodeComparison &>(*if_true.arguments[0]);
        const auto &c_false =
            static_cast<const NodeComparison &>(*if_false.arguments[0]);
        EXPECT_TRUE(c_true.alwaysTrue);
        EXPECT_TRUE(c_false.alwaysFalse);
    }

    TEST(ScriptEval, IfProcessorCollectsAffectedVars)
    {
        std::vector<Event> events = parse_script(
            "2025-01-01\n    if spot() > 100 then\n        a = 1\n        b = 2\n"
            "    else\n        c = 3\n    endIf\n");
        VarIndexer().index(events);
        IfProcessor().process(events);
        const auto &iff =
            static_cast<const NodeIf &>(*events[0].statements[0]->arguments[0]);
        EXPECT_EQ(iff.affectedVars.size(), 3u); // a, b, c
    }

    // ── spot(i): multi-asset (blueprint/wp/17-aad.md lot 17e) ───────────────

    TEST(ScriptEval, SpotWithAnIndexReadsTheCorrespondingAsset)
    {
        auto r = run_hard_multi("2025-01-01\n    a = spot(0)\n    b = spot(1)\n"
                                "    pays min(a, b)\n",
                                2, {100.0, 90.0});
        EXPECT_DOUBLE_EQ(r.var("a"), 100.0);
        EXPECT_DOUBLE_EQ(r.var("b"), 90.0);
        EXPECT_DOUBLE_EQ(r.payoff, 90.0); // worst of the two
    }

    TEST(ScriptEval, SpotWithNoIndexStillReadsAssetZeroAmongSeveral)
    {
        auto r = run_hard_multi("2025-01-01\n    a = spot()\n    b = spot(1)\n"
                                "    pays a + b\n",
                                2, {100.0, 90.0});
        EXPECT_DOUBLE_EQ(r.payoff, 190.0);
    }

    TEST(ScriptEval, SpotWithAnOutOfRangeIndexThrowsRatherThanReadingGarbage)
    {
        // The model behind this scenario only carries one asset (run_hard's
        // convention); spot(1) asks for a second one that isn't there.
        EXPECT_THROW(run_hard("2025-01-01\n    pays spot(1)\n", {100.0}),
                    InvalidInput);
    }

} // namespace quantModeling::scripting
