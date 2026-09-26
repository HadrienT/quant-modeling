#include <gtest/gtest.h>

#include <Eigen/Core>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"

// Lot G2 of blueprint/wp/19-gpu.md: the scripts compiled to bytecode price to
// the same bits as the tree evaluators -- the 42 products of the library
// (api/app/product_library), hard and fuzzy, in double and under AAD.

namespace quantModeling
{
    namespace
    {
        struct LibraryScript
        {
            std::string name;
            std::string source;
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

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};

        template <class T>
        std::unique_ptr<ISimulationModel<T>> model_for(std::size_t n)
        {
            if (n == 1)
                return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25));
            Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n), 0.5);
            corr.diagonal().setOnes();
            std::vector<T> s0, q, vol;
            for (std::size_t i = 0; i < n; ++i)
            {
                s0.push_back(T(100.0 - 5.0 * static_cast<double>(i)));
                q.push_back(T(0.01));
                vol.push_back(T(0.2 + 0.05 * static_cast<double>(i)));
            }
            return std::make_unique<MultiAssetBSSimModel<T>>(s0, T(0.03), q, vol, corr);
        }

        SimulationMCResult price(const std::string &script, bool fuzzy, bool bytecode)
        {
            ScriptSettings s;
            s.fuzzy = fuzzy;
            s.bytecode = bytecode;
            ScriptedProduct<Real> product(script, kCtx, s);
            auto model = model_for<Real>(product.n_underlyings());
            PricingSettings mc;
            mc.mc_paths = 4000;
            mc.mc_seed = 17;
            return simulate<Real>(product, *model, mc);
        }
    } // namespace

    TEST(ScriptBytecode, TheLibraryHasItsFortyTwoScripts)
    {
        EXPECT_EQ(library().size(), 42u);
    }

    TEST(ScriptBytecode, HardPricesAreBitIdenticalToTheTree)
    {
        for (const auto &s : library())
        {
            const auto tree = price(s.source, false, false);
            const auto code = price(s.source, false, true);
            EXPECT_EQ(code.npv(), tree.npv()) << s.name;
            EXPECT_EQ(code.std_error(), tree.std_error()) << s.name;
        }
    }

    TEST(ScriptBytecode, FuzzyPricesAreBitIdenticalToTheTree)
    {
        for (const auto &s : library())
        {
            const auto tree = price(s.source, true, false);
            const auto code = price(s.source, true, true);
            EXPECT_EQ(code.npv(), tree.npv()) << s.name;
            EXPECT_EQ(code.std_error(), tree.std_error()) << s.name;
        }
    }

    // Under AAD the bytecode records the same tape: every risk, to the bit.
    TEST(ScriptBytecode, AadRisksAreBitIdenticalToTheTree)
    {
        for (const auto &s : library())
            for (bool fuzzy : {false, true})
            {
                AADSimulResults r[2];
                for (int b = 0; b < 2; ++b)
                {
                    ScriptSettings settings;
                    settings.fuzzy = fuzzy;
                    settings.bytecode = (b == 1);
                    ScriptedProduct<aad::Number> product(s.source, kCtx, settings);
                    auto model = model_for<aad::Number>(product.n_underlyings());
                    r[b] = simulate_aad(product, *model, 256, 23);
                }
                EXPECT_EQ(r[1].price, r[0].price) << s.name << (fuzzy ? " fuzzy" : "");
                ASSERT_EQ(r[1].risks.size(), r[0].risks.size()) << s.name;
                for (std::size_t i = 0; i < r[0].risks.size(); ++i)
                    EXPECT_EQ(r[1].risks[i], r[0].risks[i]) << s.name << (fuzzy ? " fuzzy " : " ") << r[0].risk_labels[i];
            }
    }

    // The compiled program is flat and bounded: what a GPU thread needs.
    TEST(ScriptBytecode, ProgramShape)
    {
        for (const auto &s : library())
        {
            ScriptedProduct<Real> product(s.source, kCtx, ScriptSettings{});
            const scripting::Program &p = product.program();
            ASSERT_EQ(p.n_events(), product.timeline().size()) << s.name;
            EXPECT_GT(p.code.size(), 0u) << s.name;
            EXPECT_LE(p.max_stack, 64) << s.name;
        }
    }

} // namespace quantModeling
