#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "quantModeling/utils/dual.hpp"

#include "ir_fixtures.hpp"

// Lot L2 of blueprint/wp/27-ir-ad.md: the models' steps written in the IR.
// A path run from the IR's blocks (init, the model's step, the script's
// events on one state) is the hand-written path's (mc::script_path), bit
// for bit -- every library script, crisp and fuzzy, its numbers as constants
// and as terms, under Black-Scholes (one and several assets), local vol and
// Heston; the price's variant against the double path, the variant the
// derivatives are taken from against the value of the dual path.

namespace quantModeling
{
    namespace
    {
        using namespace ir_test;
        using scripting::ir::PathData;
        using scripting::ir::PathProgram;
        using scripting::ir::PathScratch;

        constexpr int kPaths = 48;

        struct Outcome
        {
            long long paths = 0, mismatches = 0;
            std::string first;
        };

        void expect_same_paths(Dyn dyn, bool risk)
        {
            int cases = 0;
            for (const LibraryScript &script : library())
                for (int mode = 0; mode < 4; ++mode)
                {
                    ScriptSettings ss;
                    ss.fuzzy = (mode & 1) != 0;
                    ss.terms_as_parameters = (mode & 2) != 0;
                    {
                        const ScriptedProduct<Real> probe(script.source, kCtx, ss);
                        if (probe.program().code.empty() || probe.program().has_survival)
                            continue;
                        if (dyn != Dyn::BlackScholes && probe.n_underlyings() > 1)
                            continue;
                    }
                    const Case c(script.source, dyn, ss);
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
                    Outcome o;
                    for (int p = 0; p < kPaths; ++p)
                        for (double sign : {1.0, -1.0})
                        {
                            draws.begin(static_cast<uint64_t>(p));
                            const double hand = risk ? mc::script_path(v, dual_in, draws, sign, nullptr).v
                                                     : mc::script_path(v, in, draws, sign, nullptr);
                            draws.begin(static_cast<uint64_t>(p));
                            const double ir =
                                scripting::ir::run_path(program, data, c.dm, c.product.ir(), draws, sign, scratch);
                            ++o.paths;
                            if (!same_bits(hand, ir) && o.mismatches++ == 0)
                                o.first = "path " + std::to_string(p) + ": hand-written " + std::to_string(hand) +
                                          ", IR " + std::to_string(ir);
                        }
                    EXPECT_EQ(o.mismatches, 0)
                        << script.name << " under " << name(dyn) << (ss.fuzzy ? ", fuzzy" : "")
                        << (ss.terms_as_parameters ? ", terms" : "") << ": " << o.first;
                    ++cases;
                }
            EXPECT_GT(cases, 20);
        }
    } // namespace

    TEST(ScriptIrPath, BlackScholesPriceIsTheHandWrittenPath)
    {
        expect_same_paths(Dyn::BlackScholes, false);
    }
    TEST(ScriptIrPath, LocalVolPriceIsTheHandWrittenPath)
    {
        expect_same_paths(Dyn::LocalVol, false);
    }
    TEST(ScriptIrPath, HestonPriceIsTheHandWrittenPath)
    {
        expect_same_paths(Dyn::Heston, false);
    }
    // The variant the derivatives are taken from: Black-Scholes on its
    // parameters, the discounting from the rate -- the dual path's value.
    TEST(ScriptIrPath, BlackScholesRiskVariantIsTheDualPathsValue)
    {
        expect_same_paths(Dyn::BlackScholes, true);
    }
    TEST(ScriptIrPath, HestonRiskVariantIsTheDualPathsValue)
    {
        expect_same_paths(Dyn::Heston, true);
    }

    // The model's step is small (§3.3): Black-Scholes's price step is two
    // loads of its tabulated coefficients, the draw, the state, and four
    // operations.
    TEST(ScriptIrPath, TheBlackScholesStepIsAHandfulOfOperations)
    {
        const Case c("2027-06-01\n    pays max(spot() - 100, 0)\n", Dyn::BlackScholes, ScriptSettings{});
        const PathProgram program = scripting::ir::path_program(c.product.ir(), c.dm.kind, 1, false);
        const std::string text = scripting::ir::to_string(program.step);
        EXPECT_EQ(program.step.code.size(), 8u) << text;
        for (const char *op : {"= draw 0", "= data 0", "= data 1", "= state 0", "= mul", "= add", "= exp"})
            EXPECT_NE(text.find(op), std::string::npos) << op << " in\n"
                                                        << text;
        EXPECT_NE(text.find("state_out 0 = %7"), std::string::npos) << text;
    }

} // namespace quantModeling
