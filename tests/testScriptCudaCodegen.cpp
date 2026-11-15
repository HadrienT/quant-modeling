#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/cuda_codegen.hpp"

// Lot P1b of blueprint/wp/26-performance.md, generated from the IR since lot
// L1 of blueprint/wp/27-ir-ad.md: the generator of a script's CUDA source. That the source gives the interpreter's bits on the device is
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
            const scripting::CudaSource src = scripting::cuda_path_source(product.ir(), 0, static_cast<int>(product.n_underlyings()), scripting::PathKernel::Price);
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
                const scripting::CudaSource a = scripting::cuda_path_source(product.ir(), 0, static_cast<int>(product.n_underlyings()), scripting::PathKernel::Price);
                ASSERT_TRUE(a.why.empty()) << entry.path().stem() << ": " << a.why;
                EXPECT_EQ(count(a.source, "    case "), product.ir().blocks.size()) << entry.path().stem();
                EXPECT_EQ(a.source, scripting::cuda_path_source(product.ir(), 0, static_cast<int>(product.n_underlyings()), scripting::PathKernel::Price).source);
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

    // The arithmetic goes through the intrinsics the device compiler never
    // contracts into an FMA: the interpreter never fuses them either.
    TEST(ScriptCudaCodegen, ArithmeticIsNeverContracted)
    {
        const std::string src = source_of("2027-06-01\n    pays spot() * 2 + 1 - spot()\n");
        EXPECT_NE(src.find("__dmul_rn("), std::string::npos);
        EXPECT_NE(src.find("__dadd_rn("), std::string::npos);
        EXPECT_NE(src.find("__dsub_rn("), std::string::npos);
        EXPECT_EQ(src.find(" * "), std::string::npos) << "a bare product could be contracted:\n"
                                                      << src;
    }

    // A daily schedule's dates share one body: 252 fixings, three cases --
    // the instruction cache holds the script, not a copy per date, and which
    // case a date runs is data (WP 27 lot L1b).
    TEST(ScriptCudaCodegen, ScheduleDatesShareTheirCode)
    {
        const std::string src = source_of("2026-06-02\n    acc = spot()\n    n = 1\n\n"
                                          "schedule(2026-06-03, 2027-05-31, 1D, TARGET, F)\n"
                                          "    acc = acc + spot()\n    n = n + 1\n\n"
                                          "2027-06-01\n    acc = acc + spot()\n    n = n + 1\n"
                                          "    pays max(acc / n - 100, 0)\n");
        // One case per distinct block: first date, schedule, last date.
        EXPECT_EQ(count(src, "    case "), 3u);
        EXPECT_EQ(count(src, "\n    {\n        double x0"), 3u) << "distinct bodies";
    }

    // A fuzzy if picks its mode from its degree and blends as the
    // interpreter, without a bare product either.
    TEST(ScriptCudaCodegen, FuzzyIfsGenerate)
    {
        const std::string src = source_of("2027-06-01\n    if spot() > 100 then pays 1 else pays 0 endIf\n", true);
        EXPECT_NE(src.find("qm_up("), std::string::npos) << src;
        EXPECT_NE(src.find("<= 0.0 ? 1 : ("), std::string::npos) << src;
        EXPECT_EQ(src.find(" * "), std::string::npos) << src;
    }

    // What cannot be generated is refused with a reason (the interpreter
    // runs): no IR, or a script that reads the bridge survival.
    TEST(ScriptCudaCodegen, WhatCannotBeGeneratedIsRefused)
    {
        EXPECT_FALSE(scripting::cuda_path_source(scripting::ir::Script{}, 0, 1, scripting::PathKernel::Price).why.empty());
        const ScriptedProduct<Real> product("2027-06-01\n    pays survivalAbove(80) * spot()\n", kCtx);
        const scripting::CudaSource src = scripting::cuda_path_source(product.ir(), 0, static_cast<int>(product.n_underlyings()), scripting::PathKernel::Price);
        EXPECT_TRUE(src.source.empty());
        EXPECT_NE(src.why.find("survival"), std::string::npos) << src.why;
    }

    // Lot L1b of blueprint/wp/27-ir-ad.md: with its numbers as terms, a
    // product's kernel depends on its shape alone -- another strike, another
    // barrier, another number of dates give the same source, hence the same
    // compiled kernel.
    TEST(ScriptCudaCodegen, TermsAndDatesAreDataNotCode)
    {
        const auto up_and_out = [](double strike, double barrier, int months, bool terms)
        {
            std::string script = "2026-07-01\n    alive = 1\n";
            for (int m = 1; m <= months; ++m)
            {
                char date[16];
                std::snprintf(date, sizeof date, "20%02d-%02d-01", 26 + (6 + m) / 12, 1 + (6 + m) % 12);
                script += std::string(date) + "\n    if spot() >= " + std::to_string(barrier) +
                          " then alive = 0 endIf\n";
            }
            script += "2030-01-02\n    pays alive * max(spot() - " + std::to_string(strike) + ", 0)\n";
            ScriptSettings ss;
            ss.terms_as_parameters = terms;
            const ScriptedProduct<Real> product(script, kCtx, ss);
            const scripting::CudaSource src = scripting::cuda_path_source(product.ir(), 0, static_cast<int>(product.n_underlyings()), scripting::PathKernel::Price);
            EXPECT_TRUE(src.why.empty()) << src.why;
            return src.source;
        };
        const std::string a = up_and_out(100.0, 130.0, 12, true);
        EXPECT_EQ(a, up_and_out(95.0, 140.0, 12, true)) << "other terms";
        EXPECT_EQ(a, up_and_out(100.0, 130.0, 30, true)) << "other dates";
        EXPECT_NE(a.find("f->tm["), std::string::npos) << a;
        // A script with its numbers as constants: the terms are in the code.
        EXPECT_NE(up_and_out(100.0, 130.0, 12, false), up_and_out(95.0, 140.0, 12, false));
        // ... but its dates are data already.
        EXPECT_EQ(up_and_out(100.0, 130.0, 12, false), up_and_out(100.0, 130.0, 30, false));
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
