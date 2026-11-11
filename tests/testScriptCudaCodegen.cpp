#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/cuda_codegen.hpp"

// Lot P1b of blueprint/wp/26-performance.md: the generator of a script's CUDA
// source. That the source gives the interpreter's bits on the device is
// tests/gpu/testGpuScriptJit.cpp's job (the CI has no GPU); here, what the
// generator promises without one.

namespace quantModeling
{
    namespace
    {
        const ValuationContext kCtx{Date::from_iso("2026-06-01")};

        std::size_t count(const std::string &text, const std::string &what)
        {
            std::size_t n = 0;
            for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
                ++n;
            return n;
        }

        std::string source_of(const std::string &script, bool fuzzy = false)
        {
            ScriptSettings ss;
            ss.fuzzy = fuzzy;
            const ScriptedProduct<Real> product(script, kCtx, ss);
            const scripting::CudaSource src = scripting::cuda_event_source(product.program());
            EXPECT_TRUE(src.why.empty()) << src.why;
            return src.source;
        }
    } // namespace

    // Every library script, crisp and fuzzy, generates; the same program
    // gives the same source (the cache key of the compiled code).
    TEST(ScriptCudaCodegen, EveryLibraryScriptGenerates)
    {
        int n = 0;
        for (const auto &entry : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
        {
            if (entry.path().extension() != ".qms")
                continue;
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            for (bool fuzzy : {false, true})
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(text.str(), kCtx, ss);
                const scripting::CudaSource a = scripting::cuda_event_source(product.program());
                ASSERT_TRUE(a.why.empty()) << entry.path().stem() << ": " << a.why;
                EXPECT_EQ(count(a.source, "    case "), product.program().n_events()) << entry.path().stem();
                EXPECT_EQ(a.source, scripting::cuda_event_source(product.program()).source);
            }
            ++n;
        }
        EXPECT_GT(n, 40);
    }

    // Constants are written to the bit: 0.1 is 0x1.999999999999ap-4, not
    // a decimal the device compiler would round again.
    TEST(ScriptCudaCodegen, ConstantsAreExact)
    {
        const std::string src = source_of("2027-06-01\n    pays 0.1 * spot()\n");
        EXPECT_NE(src.find("0x1.999999999999ap-4"), std::string::npos) << src;
    }

    // The stack arithmetic goes through the intrinsics the device compiler
    // never contracts into an FMA: the interpreter never fuses them either.
    TEST(ScriptCudaCodegen, StackArithmeticIsNeverContracted)
    {
        const std::string src = source_of("2027-06-01\n    pays spot() * 2 + 1 - spot()\n");
        EXPECT_NE(src.find("__dmul_rn("), std::string::npos);
        EXPECT_NE(src.find("__dadd_rn("), std::string::npos);
        EXPECT_NE(src.find("__dsub_rn("), std::string::npos);
        EXPECT_EQ(src.find(" * "), std::string::npos) << "a bare product could be contracted:\n"
                                                      << src;
    }

    // A daily schedule's dates share one body: 252 fixings, a handful of
    // distinct cases -- the instruction cache holds the script, not a copy
    // per date.
    TEST(ScriptCudaCodegen, ScheduleDatesShareTheirCode)
    {
        const std::string src = source_of("2026-06-02\n    acc = spot()\n    n = 1\n\n"
                                          "schedule(2026-06-03, 2027-05-31, 1D, TARGET, F)\n"
                                          "    acc = acc + spot()\n    n = n + 1\n\n"
                                          "2027-06-01\n    acc = acc + spot()\n    n = n + 1\n"
                                          "    pays max(acc / n - 100, 0)\n");
        EXPECT_GT(count(src, "    case "), 200u);
        // A body opens on its stack slots: first date, schedule, last date.
        EXPECT_EQ(count(src, "\n    {\n        double s0"), 3u) << "distinct bodies";
    }

    // Fuzzy ifs keep their state in locals and blend as the interpreter.
    TEST(ScriptCudaCodegen, FuzzyIfsGenerate)
    {
        const std::string src = source_of("2027-06-01\n    if spot() > 100 then pays 1 else pays 0 endIf\n", true);
        EXPECT_NE(src.find("qm_up("), std::string::npos) << src;
        EXPECT_NE(src.find("w1 = 1.0 - degree"), std::string::npos) << src;
    }

    // A malformed program is refused with a reason (the interpreter runs).
    TEST(ScriptCudaCodegen, MalformedProgramIsRefused)
    {
        const ScriptedProduct<Real> product("2027-06-01\n    pays spot()\n", kCtx);
        scripting::Program p = product.program();
        p.code.insert(p.code.begin() + p.event_begin[0], scripting::Instr{scripting::Op::Add, 0, 0, 0.0});
        for (std::size_t e = 1; e < p.event_begin.size(); ++e)
            ++p.event_begin[e];
        const scripting::CudaSource src = scripting::cuda_event_source(p);
        EXPECT_TRUE(src.source.empty());
        EXPECT_NE(src.why.find("underflows"), std::string::npos) << src.why;

        scripting::Program empty;
        EXPECT_FALSE(scripting::cuda_event_source(empty).why.empty());
    }

    TEST(ScriptCudaCodegen, SpecIsWrittenAsConstants)
    {
        scripting::CudaKernelSpec spec;
        spec.model_kind = 1;
        spec.n_assets = 3;
        spec.sobol = true;
        spec.antithetic = false;
        const std::string src = scripting::cuda_spec_source(spec);
        EXPECT_NE(src.find("qm_spec_kind() { return 1; }"), std::string::npos) << src;
        EXPECT_NE(src.find("qm_spec_n_assets() { return 3; }"), std::string::npos);
        EXPECT_NE(src.find("qm_spec_sobol() { return 1; }"), std::string::npos);
        EXPECT_NE(src.find("qm_spec_antithetic() { return 0; }"), std::string::npos);
    }

} // namespace quantModeling
