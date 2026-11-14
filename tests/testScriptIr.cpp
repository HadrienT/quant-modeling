#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/cuda_codegen.hpp"
#include "quantModeling/scripting/ir.hpp"

// Lot L1 of blueprint/wp/27-ir-ad.md: a script lowered to the IR computes
// what its bytecode computes, bit for bit -- every library script, crisp and
// fuzzy, event by event on random market states. The CI has no GPU: this is
// where the IR's semantics are pinned; tests/gpu/testGpuScriptJit.cpp checks
// the CUDA generated from it on the device.

namespace quantModeling
{
    namespace
    {
        const ValuationContext kCtx{Date::from_iso("2026-06-01")};

        struct LibraryScript
        {
            std::string name, source;
        };

        std::vector<LibraryScript> library()
        {
            std::vector<LibraryScript> out;
            for (const auto &entry : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
            {
                if (entry.path().extension() != ".qms")
                    continue;
                std::ifstream in(entry.path());
                std::stringstream text;
                text << in.rdbuf();
                out.push_back({entry.path().stem().string(), text.str()});
            }
            std::sort(out.begin(), out.end(), [](const auto &a, const auto &b)
                      { return a.name < b.name; });
            return out;
        }

        bool same_bits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }

        /// What the two machines saw over `paths` random paths.
        struct Comparison
        {
            long long values = 0;     ///< variables and payoffs compared
            long long mismatches = 0; ///< of which not the same bits
            long long blends = 0;     ///< fuzzy ifs that ran both blocks
            std::string first;        ///< where the first mismatch is
        };

        Comparison compare(const ScriptedProduct<Real> &product, int paths, unsigned seed)
        {
            using namespace scripting;
            const Program &p = product.program();
            const ir::Script &s = product.ir();
            Comparison out;
            const std::size_t n_assets = std::max<std::size_t>(product.n_underlyings(), 1);
            std::mt19937_64 gen(seed);
            std::normal_distribution<double> normal;
            std::uniform_real_distribution<double> uniform(0.05, 0.999);

            const auto nv = static_cast<std::size_t>(p.n_vars);
            std::vector<double> vars_b(nv), vars_i(nv), stack(static_cast<std::size_t>(std::max(1, p.max_stack))),
                degrees(static_cast<std::size_t>(std::max(1, p.max_degrees))),
                slots(static_cast<std::size_t>(std::max(1, p.n_if_slots)));
            std::vector<int> modes(static_cast<std::size_t>(std::max(1, p.n_if_modes)));
            std::size_t longest = 1;
            for (const ir::Block &b : s.blocks)
                longest = std::max(longest, b.code.size());
            std::vector<double> val(longest, 0.0);
            std::vector<double> spots(n_assets), dfs(16), survs(16);
            const std::vector<Real> baseline = product.baseline();

            for (int path = 0; path < paths; ++path)
            {
                for (std::size_t i = 0; i < nv; ++i)
                    vars_b[i] = vars_i[i] = baseline.empty() ? 0.0 : baseline[i];
                Machine<double> m{vars_b.data(), stack.data(), degrees.data(), slots.data(), modes.data(), 0.0};
                double payoff = 0.0;
                for (std::size_t k = 0; k < n_assets; ++k)
                    spots[k] = 100.0 - 4.0 * static_cast<double>(k);
                for (std::size_t e = 0; e < p.n_events(); ++e)
                {
                    for (std::size_t k = 0; k < n_assets; ++k)
                        spots[k] *= std::exp(0.08 * normal(gen));
                    for (double &d : dfs)
                        d = uniform(gen);
                    for (double &d : survs)
                        d = uniform(gen);
                    const double numeraire = 1.0 + 0.2 * uniform(gen);
                    run_event(p.view(), p.event_begin[e], p.event_begin[e + 1], m, spots.data(), dfs.data(),
                              numeraire, survs.data());
                    const ir::Block &b = s.event(e);
                    ir::run_block(b, vars_i.data(), spots.data(), dfs.data(), numeraire, payoff, survs.data(),
                                  val.data(), s.event_terms[e].data());
                    for (const ir::Inst &in : b.code)
                        if (in.op == ir::Op::FuzzyIf && val[static_cast<std::size_t>(&in - b.code.data())] == 2.0)
                            ++out.blends;
                    for (std::size_t i = 0; i <= nv; ++i)
                    {
                        const double x = i < nv ? vars_b[i] : m.payoff, y = i < nv ? vars_i[i] : payoff;
                        ++out.values;
                        if (same_bits(x, y))
                            continue;
                        if (out.mismatches++ == 0)
                            out.first = "path " + std::to_string(path) + ", event " + std::to_string(e) +
                                        (i < nv ? ", variable " + std::to_string(i) : ", payoff") + ": bytecode " +
                                        std::to_string(x) + ", IR " + std::to_string(y);
                    }
                }
            }
            return out;
        }
    } // namespace

    TEST(ScriptIr, EveryLibraryScriptComputesTheBytecodesBits)
    {
        int scripts = 0;
        long long blends = 0;
        for (const LibraryScript &script : library())
            for (int mode = 0; mode < 4; ++mode)
            {
                // Crisp and fuzzy; the numbers as constants and as terms
                // (lot L1b: the same doubles, read from data).
                const bool fuzzy = (mode & 1) != 0, terms = (mode & 2) != 0;
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                ss.terms_as_parameters = terms;
                const ScriptedProduct<Real> product(script.source, kCtx, ss);
                if (product.program().code.empty()) // early exercise: the tree evaluator
                {
                    EXPECT_TRUE(product.ir().empty()) << script.name;
                    continue;
                }
                ASSERT_EQ(product.ir().n_events(), product.program().n_events()) << script.name;
                ASSERT_EQ(product.ir().event_terms.size(), product.ir().n_events()) << script.name;
                const Comparison c = compare(product, 400, 7u);
                EXPECT_EQ(c.mismatches, 0) << script.name << (fuzzy ? " (fuzzy" : " (crisp")
                                           << (terms ? ", terms): " : "): ") << c.first;
                EXPECT_GT(c.values, 0);
                blends += c.blends;
                ++scripts;
            }
        EXPECT_GT(scripts, 80);
        EXPECT_GT(blends, 1000) << "the fuzzy ifs' blended mode was hardly exercised";
    }

    // A schedule's dates run the same statements: one block, however many.
    TEST(ScriptIr, DatesWithTheSameStatementsShareOneBlock)
    {
        std::string script;
        for (int month = 1; month <= 12; ++month)
        {
            char date[16];
            std::snprintf(date, sizeof date, "2027-%02d-15", month);
            script += std::string(date) + "\n    acc = acc + spot()\n";
        }
        script += "2028-01-17\n    pays max(acc / 12 - 100, 0)\n";
        const ScriptedProduct<Real> product(script, kCtx);
        EXPECT_EQ(product.ir().n_events(), 13u);
        EXPECT_EQ(product.ir().blocks.size(), 2u);
    }

    // The passes: a repeated subexpression is computed once, an operation on
    // constants is folded when the fold is exact, and what nothing reads is
    // gone.
    TEST(ScriptIr, CommonSubexpressionsFoldsAndDeadCode)
    {
        const ScriptedProduct<Real> product("2027-06-01\n"
                                            "    unused = spot() * 3\n"
                                            "    a = (spot() - 100) * (spot() - 100)\n"
                                            "    pays a * (2 * 0.5)\n",
                                            kCtx);
        const scripting::ir::Block &b = product.ir().blocks.at(0);
        const std::string text = scripting::ir::to_string(b);
        const auto count = [&](scripting::ir::Op op)
        { return std::count_if(b.code.begin(), b.code.end(), [&](const auto &in)
                               { return in.op == op; }); };
        EXPECT_EQ(count(scripting::ir::Op::Spot), 1) << text;
        EXPECT_EQ(count(scripting::ir::Op::Sub), 1) << text;
        // (x - 100)^2, then times the folded 1: `unused` still leaves the event.
        EXPECT_EQ(count(scripting::ir::Op::Mul), 3) << text;
        EXPECT_NE(text.find("const 1\n"), std::string::npos) << text;
        EXPECT_EQ(text.find("const 0.5"), std::string::npos) << text;
    }

    // exp, log and pow are not folded: their last bit is the library's.
    TEST(ScriptIr, TranscendentalsAreNotFolded)
    {
        const ScriptedProduct<Real> product("2027-06-01\n    pays spot() * exp(0.5)\n", kCtx);
        const scripting::ir::Block &b = product.ir().blocks.at(0);
        EXPECT_TRUE(std::any_of(b.code.begin(), b.code.end(),
                                [](const auto &in)
                                { return in.op == scripting::ir::Op::Exp; }))
            << scripting::ir::to_string(b);
    }

    // The printed form of the design's example (§3.1).
    TEST(ScriptIr, PrintsTheDesignsExample)
    {
        const ScriptedProduct<Real> product("2027-01-04\n    acc = acc + spot()\n    pays max(acc / 252 - 100, 0)\n",
                                            kCtx);
        EXPECT_EQ(scripting::ir::to_string(product.ir().blocks.at(0)), "  %0 = var_in 0\n"
                                                                       "  %1 = pay_in\n"
                                                                       "  %2 = load_spot 0\n"
                                                                       "  %3 = add %0, %2\n"
                                                                       "  %4 = const 252\n"
                                                                       "  %5 = div %3, %4\n"
                                                                       "  %6 = const 100\n"
                                                                       "  %7 = sub %5, %6\n"
                                                                       "  %8 = const 0\n"
                                                                       "  %9 = max %7, %8\n"
                                                                       "  %10 = load_numeraire\n"
                                                                       "  %11 = div %9, %10\n"
                                                                       "  %12 = add %1, %11\n"
                                                                       "  var_out 0 = %3\n"
                                                                       "  pay_out = %12\n");
    }

    // The CUDA generated from the IR: one case per block, no user text.
    TEST(ScriptIr, CudaSourceSharesTheCodeOfSharedBlocks)
    {
        const ScriptedProduct<Real> product("2027-01-04 2027-02-04 2027-03-04\n    acc = acc + spot()\n"
                                            "2027-04-05\n    pays max(acc / 3 - 100, 0)\n",
                                            kCtx);
        const scripting::CudaSource src = scripting::cuda_path_source(product.ir(), 0, 1, scripting::PathKernel::Price);
        ASSERT_TRUE(src.why.empty()) << src.why;
        EXPECT_NE(src.source.find("case 0:\n    {"), std::string::npos) << src.source;
        EXPECT_NE(src.source.find("case 1:\n    {"), std::string::npos) << src.source;
        EXPECT_EQ(src.source.find("case 2:"), std::string::npos) << src.source;
        EXPECT_EQ(src.source.find("acc ="), std::string::npos);
    }

} // namespace quantModeling
