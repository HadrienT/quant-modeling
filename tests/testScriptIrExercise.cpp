#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/utils/dual.hpp"

#include "ir_fixtures.hpp"

// WP 26 §16 (#282 step 3): exercise() / call() in the IR. Once the rule is
// fitted, a path run from the IR's blocks is the bytecode's path under the
// same rule (mc::script_path with the rule in its view), bit for bit -- hard
// and fuzzy, the numbers as constants and as terms, under Black-Scholes,
// local vol and Heston; the risks' variant against the dual path's value.

namespace quantModeling
{
    namespace
    {
        using namespace ir_test;
        using scripting::ir::PathData;
        using scripting::ir::PathProgram;
        using scripting::ir::PathScratch;

        const std::vector<std::string> &scripts()
        {
            static const std::vector<std::string> s = {
                // Bermudan put, the exercise value and the spot as regressors
                "2026-07-01 2026-08-03 2026-09-01 2026-10-01 2026-11-02 2026-12-01 2027-01-04 2027-02-01\n"
                "    if exercise(max(100 - spot(), 0), spot()) then\n        pays max(100 - spot(), 0)\n"
                "    endIf\n",
                // no argument: the event's spots
                "2026-09-01 2026-12-01 2027-03-01 2027-06-01\n    if exercise() then\n"
                "        pays max(100 - spot(), 0)\n    endIf\n",
                // the decision sets a variable read later (chooser)
                "2026-12-01\n    if exercise(spot()) then\n        is_call = 1\n    endIf\n"
                "2027-06-01\n    if is_call = 1 then\n        pays max(spot() - 100, 0)\n    else\n"
                "        pays max(100 - spot(), 0)\n    endIf\n",
                // the issuer's call nested in an if, a smoothed condition after it
                "2026-12-01 2027-06-01 2027-12-01\n    if done = 0 then\n        pays 2\n"
                "        if call(spot()) then\n            pays 100\n            done = 1\n        endIf\n"
                "    endIf\n2028-06-01\n    if done = 0 then\n        if spot() > 90 then\n"
                "            pays 104\n        else\n            pays 104 * spot() / 90\n        endIf\n    endIf\n",
                // the decision negated and combined
                "2026-09-01 2026-12-01 2027-03-01 2027-06-01\n"
                "    if not exercise(spot()) and spot() > 70 then\n        alive = 1\n    else\n"
                "        pays max(100 - spot(), 0)\n    endIf\n",
                // inside an if on the spot: fuzzy, an interior degree runs
                // both branches and the right stays exercised unblended
                "2026-09-01 2026-12-01 2027-03-01 2027-06-01\n    if spot() < 98 then\n"
                "        if exercise(spot()) then\n            pays 100 - spot()\n        endIf\n    endIf\n"};
            return s;
        }

        /// A case whose rule is fitted, so that its IR exists.
        struct ExerciseCase : Case
        {
            scripting::ExerciseView rule;
            ExerciseCase(const std::string &source, Dyn dyn, const ScriptSettings &ss)
                : Case(source, dyn, ss)
            {
                fit_exercise_policy(product, *model, 8192, 5);
                rule = product.exercise_rule();
                host->view.exercise = &rule;
            }
        };

        void expect_same_paths(Dyn dyn, bool risk)
        {
            int cases = 0;
            for (const std::string &source : scripts())
                for (int mode = 0; mode < 4; ++mode)
                {
                    ScriptSettings ss;
                    ss.fuzzy = (mode & 1) != 0;
                    ss.terms_as_parameters = (mode & 2) != 0;
                    const ExerciseCase c(source, dyn, ss);
                    ASSERT_FALSE(c.product.ir().empty()) << c.product.ir_refusal() << "\n"
                                                         << source;
                    ASSERT_TRUE(c.product.ir().has_exercise);
                    ASSERT_FALSE(c.product.exercise_policy().empty()) << source;
                    const mc::ScriptPathView &v = c.host->view;
                    const PathProgram program =
                        scripting::ir::path_program(c.product.ir(), c.dm.kind, c.dm.n_assets, risk);
                    const PathData data =
                        scripting::ir::path_data(program.layout, c.dm, c.product.ir(), c.mats, c.base);
                    PathScratch scratch(program);
                    const auto in = mc::script_inputs<double>(v);
                    const auto dual_in = mc::script_inputs<Dual<1>>(v);
                    mc::PathDraws draws;
                    draws.seed = 23;
                    draws.stride = v.stride;
                    long long mismatches = 0, exercised = 0;
                    std::string first;
                    for (int p = 0; p < 200; ++p)
                        for (double sign : {1.0, -1.0})
                        {
                            draws.begin(static_cast<uint64_t>(p));
                            const double hand = risk ? mc::script_path(v, dual_in, draws, sign, nullptr).v
                                                     : mc::script_path(v, in, draws, sign, nullptr);
                            draws.begin(static_cast<uint64_t>(p));
                            const double ir =
                                scripting::ir::run_path(program, data, c.dm, c.product.ir(), draws, sign, scratch);
                            // the hidden variable: whether the right was exercised
                            exercised += scratch.state[static_cast<std::size_t>(program.layout.var0() + c.product.ir().n_vars - 1)] != 0.0;
                            if (!same_bits(hand, ir) && mismatches++ == 0)
                                first = "path " + std::to_string(p) + ": bytecode " + std::to_string(hand) +
                                        ", IR " + std::to_string(ir);
                        }
                    EXPECT_EQ(mismatches, 0) << name(dyn) << (ss.fuzzy ? ", fuzzy" : "")
                                             << (ss.terms_as_parameters ? ", terms" : "") << ": " << first << "\n"
                                             << source;
                    EXPECT_GT(exercised, 0) << name(dyn) << ": no path exercised\n"
                                            << source;
                    ++cases;
                }
            EXPECT_EQ(cases, 24);
        }
    } // namespace

    TEST(ScriptIrExercise, BlackScholesPriceIsTheBytecodes)
    {
        expect_same_paths(Dyn::BlackScholes, false);
    }
    TEST(ScriptIrExercise, LocalVolPriceIsTheBytecodes)
    {
        expect_same_paths(Dyn::LocalVol, false);
    }
    TEST(ScriptIrExercise, HestonPriceIsTheBytecodes)
    {
        expect_same_paths(Dyn::Heston, false);
    }
    TEST(ScriptIrExercise, BlackScholesRiskVariantIsTheDualPathsValue)
    {
        expect_same_paths(Dyn::BlackScholes, true);
    }

    // The rule's numbers are terms: two rules of one shape give one code.
    TEST(ScriptIrExercise, TheRuleIsDataNotCode)
    {
        const std::string &source = scripts()[0];
        ExerciseCase a(source, Dyn::BlackScholes, {});
        ExerciseCase b(source, Dyn::LocalVol, {});
        ASSERT_EQ(a.product.ir().blocks.size(), b.product.ir().blocks.size());
        for (std::size_t i = 0; i < a.product.ir().blocks.size(); ++i)
            EXPECT_TRUE(a.product.ir().blocks[i] == b.product.ir().blocks[i]) << "block " << i;
        EXPECT_NE(a.product.ir().event_terms, b.product.ir().event_terms);
        // the dates of the schedule share their block
        EXPECT_EQ(a.product.ir().blocks.size(), 1u);
    }

    // What the IR cannot follow stays on the CPU, saying why.
    TEST(ScriptIrExercise, WhatTheIrCannotFollowIsRefused)
    {
        {
            ScriptedProduct<Real> p(scripts()[0], kCtx);
            EXPECT_TRUE(p.ir().empty()) << "no rule yet";
            EXPECT_NE(p.ir_refusal().find("fitted"), std::string::npos) << p.ir_refusal();
        }
        {
            const std::string twice = "2026-12-01\n    if exercise(spot()) then\n        pays 1\n    endIf\n"
                                      "    if spot() > 100000 then\n        if exercise(spot()) then\n"
                                      "            pays 2\n        endIf\n    endIf\n";
            ScriptedProduct<Real> p(twice, kCtx);
            auto m = make_model<Real>(Dyn::BlackScholes, 1);
            fit_exercise_policy(p, *m, 4096, 5);
            EXPECT_TRUE(p.ir().empty());
            EXPECT_NE(p.ir_refusal().find("one event"), std::string::npos) << p.ir_refusal();
        }
    }

} // namespace quantModeling
