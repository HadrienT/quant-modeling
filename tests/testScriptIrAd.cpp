#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/utils/dual.hpp"

#include "ir_fixtures.hpp"

// Lots L3 and L4 of blueprint/wp/27-ir-ad.md: the derivatives built from the
// IR by transformation, on the host (the CI has no GPU), path by path:
//   - the tangent against the dual numbers it replaces (mc::script_path on
//     Dual<N>), Black-Scholes with one and several assets and Heston;
//   - the adjoint against the tangent (two transformations that share only
//     the table of local derivatives);
//   - the adjoint under local vol against the adjoint written by hand
//     (mc::script_lv_adjoint_path), every point of the grid;
//   - the adjoint against the CPU tape (simulate_aad), the oracle.
// Every library script, crisp and fuzzy.

namespace quantModeling
{
    namespace
    {
        using namespace ir_test;
        namespace ir = scripting::ir;

        constexpr int kPaths = 64;
        constexpr std::uint64_t kSeed = 19;

        std::vector<std::int32_t> all_risks(const ir::PathLayout &l)
        {
            std::vector<std::int32_t> d(static_cast<std::size_t>(l.n_risks));
            std::iota(d.begin(), d.end(), 0);
            return d;
        }

        bool skip(const std::string &source, const ScriptSettings &ss, Dyn dyn)
        {
            const ScriptedProduct<Real> probe(source, kCtx, ss);
            return probe.program().code.empty() || probe.program().has_survival ||
                   (dyn != Dyn::BlackScholes && probe.n_underlyings() > 1);
        }

        /// |a - b| against the largest of the row (a zero risk next to large
        /// ones is compared on their scale).
        void expect_rows_near(const std::vector<double> &a, const std::vector<double> &b, double rel,
                              const std::string &what)
        {
            ASSERT_EQ(a.size(), b.size()) << what;
            double scale = 1e-12;
            for (double x : b)
                scale = std::max(scale, std::abs(x));
            for (std::size_t i = 0; i < a.size(); ++i)
                ASSERT_NEAR(a[i], b[i], rel * scale) << what << ", risk " << i;
        }

        template <int N>
        void tangent_against_duals(Dyn dyn)
        {
            int cases = 0;
            for (const LibraryScript &script : library())
                for (bool fuzzy : {false, true})
                {
                    ScriptSettings ss;
                    ss.fuzzy = fuzzy;
                    if (skip(script.source, ss, dyn))
                        continue;
                    const Case c(script.source, dyn, ss);
                    const mc::ScriptPathView &v = c.host->view;
                    const int dirs = mc::dual_directions(c.dm.kind, c.dm.n_assets);
                    ASSERT_LE(dirs, N);
                    const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, c.dm.n_assets, true);
                    ASSERT_EQ(primal.layout.n_risks, dirs);
                    const ir::PathProgram tan = ir::tangent(primal, all_risks(primal.layout));
                    const ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
                    const auto in = mc::script_dual_inputs<N>(v);
                    ir::DerivativeScratch w;
                    mc::PathDraws draws;
                    draws.seed = kSeed;
                    draws.stride = v.stride;
                    const std::string what = script.name + " under " + name(dyn) + (fuzzy ? ", fuzzy" : "");
                    for (int p = 0; p < kPaths; ++p)
                    {
                        draws.begin(static_cast<uint64_t>(p));
                        const Dual<N> dual = mc::script_path(v, in, draws, 1.0, nullptr);
                        draws.begin(static_cast<uint64_t>(p));
                        std::vector<double> got(static_cast<std::size_t>(dirs));
                        const double price =
                            ir::run_path_tangent(tan, dirs, data, c.dm, c.product.ir(), draws, 1.0, w, got.data());
                        ASSERT_TRUE(same_bits(price, dual.v)) << what << ", path " << p;
                        expect_rows_near(got, std::vector<double>(dual.d, dual.d + dirs), 1e-12,
                                         what + ", path " + std::to_string(p));
                    }
                    ++cases;
                }
            EXPECT_GT(cases, 10);
        }

        void adjoint_against_tangent(Dyn dyn)
        {
            int cases = 0;
            for (const LibraryScript &script : library())
                for (bool fuzzy : {false, true})
                {
                    ScriptSettings ss;
                    ss.fuzzy = fuzzy;
                    if (skip(script.source, ss, dyn))
                        continue;
                    const Case c(script.source, dyn, ss);
                    const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, c.dm.n_assets, true);
                    const ir::PathProgram tan = ir::tangent(primal, all_risks(primal.layout));
                    const ir::PathProgram adj = ir::adjoint(primal);
                    const ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
                    const auto K = static_cast<std::size_t>(primal.layout.n_risks);
                    ir::DerivativeScratch w;
                    mc::PathDraws draws;
                    draws.seed = kSeed;
                    draws.stride = c.host->view.stride;
                    const std::string what = script.name + " under " + name(dyn) + (fuzzy ? ", fuzzy" : "");
                    for (int p = 0; p < kPaths; ++p)
                    {
                        std::vector<double> forward(K), gradient(data.theta.size(), 0.0);
                        draws.begin(static_cast<uint64_t>(p));
                        const double a = ir::run_path_tangent(tan, static_cast<int>(K), data, c.dm, c.product.ir(),
                                                              draws, 1.0, w, forward.data());
                        draws.begin(static_cast<uint64_t>(p));
                        const double b = ir::run_path_adjoint(primal, adj, data, c.dm, c.product.ir(), draws, 1.0, 1.0,
                                                              w, gradient.data());
                        ASSERT_TRUE(same_bits(a, b)) << what << ", path " << p;
                        gradient.resize(K);
                        expect_rows_near(gradient, forward, 1e-11, what + ", path " + std::to_string(p));
                    }
                    ++cases;
                }
            EXPECT_GT(cases, 10);
        }

        void adjoint_against_tape(Dyn dyn)
        {
            constexpr std::size_t paths = 256; // a multiple of simulate_aad's batch of 64
            int cases = 0;
            for (const LibraryScript &script : library())
                for (bool fuzzy : {false, true})
                {
                    ScriptSettings ss;
                    ss.fuzzy = fuzzy;
                    if (skip(script.source, ss, dyn))
                        continue;
                    const Case c(script.source, dyn, ss);
                    const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, c.dm.n_assets, true);
                    const ir::PathProgram adj = ir::adjoint(primal);
                    const ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
                    ir::DerivativeScratch w;
                    mc::PathDraws draws;
                    draws.seed = kSeed;
                    draws.stride = c.host->view.stride;
                    std::vector<double> gradient(data.theta.size(), 0.0);
                    double price = 0.0;
                    for (std::size_t p = 0; p < paths; ++p)
                    {
                        draws.begin(p);
                        price += ir::run_path_adjoint(primal, adj, data, c.dm, c.product.ir(), draws, 1.0,
                                                      1.0 / static_cast<double>(paths), w, gradient.data());
                    }
                    price /= static_cast<double>(paths);

                    const ScriptedProduct<aad::Number> tape_product(script.source, kCtx, ss);
                    auto tape_model = make_model<aad::Number>(dyn, c.product.n_underlyings());
                    const AADSimulResults cpu =
                        simulate_aad(tape_product, *tape_model, paths, kSeed, first_aad_payoff, RngKind::Philox);
                    const std::string what = script.name + " under " + name(dyn) + (fuzzy ? ", fuzzy" : "");
                    EXPECT_NEAR(price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << what;
                    // The tape's labels: the layout's risks, then (local vol)
                    // the grid; Heston's three jump risks, zero, are last.
                    std::vector<double> got(cpu.risks.size(), 0.0);
                    const std::size_t n_risks = static_cast<std::size_t>(primal.layout.n_risks);
                    for (std::size_t i = 0; i < got.size(); ++i)
                        if (i < n_risks || primal.layout.th_grid >= 0)
                        {
                            const std::size_t at = i < n_risks ? i
                                                               : static_cast<std::size_t>(primal.layout.th_grid) +
                                                                     (i - n_risks);
                            got[i] = at < gradient.size() ? gradient[at] : 0.0;
                        }
                    expect_rows_near(got, cpu.risks, 1e-9, what);
                    ++cases;
                }
            EXPECT_GT(cases, 10);
        }
    } // namespace

    TEST(ScriptIrTangent, BlackScholesIsTheDualPath)
    {
        tangent_against_duals<32>(Dyn::BlackScholes);
    }
    TEST(ScriptIrTangent, HestonIsTheDualPath)
    {
        tangent_against_duals<8>(Dyn::Heston);
    }

    TEST(ScriptIrAdjoint, BlackScholesAgreesWithTheTangent)
    {
        adjoint_against_tangent(Dyn::BlackScholes);
    }
    TEST(ScriptIrAdjoint, HestonAgreesWithTheTangent)
    {
        adjoint_against_tangent(Dyn::Heston);
    }
    TEST(ScriptIrAdjoint, LocalVolAgreesWithTheTangent)
    {
        adjoint_against_tangent(Dyn::LocalVol);
    }

    TEST(ScriptIrAdjoint, BlackScholesIsTheTapesGradient)
    {
        adjoint_against_tape(Dyn::BlackScholes);
    }
    TEST(ScriptIrAdjoint, HestonIsTheTapesGradient)
    {
        adjoint_against_tape(Dyn::Heston);
    }
    TEST(ScriptIrAdjoint, LocalVolIsTheTapesGradient)
    {
        adjoint_against_tape(Dyn::LocalVol);
    }

    // Local vol: the adjoint built by transformation against the one written
    // by hand for every bytecode instruction and for the model's step -- the
    // same price to the bit and the same gradient, path by path, every point
    // of the grid.
    TEST(ScriptIrAdjoint, LocalVolIsTheHandWrittenAdjointPathByPath)
    {
        int cases = 0;
        for (const LibraryScript &script : library())
            for (bool fuzzy : {false, true})
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                if (skip(script.source, ss, Dyn::LocalVol))
                    continue;
                const Case c(script.source, Dyn::LocalVol, ss);
                const mc::ScriptPathView &v = c.host->view;
                const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, 1, true);
                const ir::PathProgram adj = ir::adjoint(primal);
                const ir::PathData data = ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
                const std::size_t n_params = data.theta.size();
                ASSERT_EQ(n_params, 3 + kK.size() * kT.size());
                std::vector<double> trail(static_cast<std::size_t>(mc::lv_trail_bound(c.product.program(), v.n_steps)));
                ir::DerivativeScratch w;
                mc::PathDraws draws;
                draws.seed = kSeed;
                const std::string what = script.name + (fuzzy ? ", fuzzy" : "");
                for (int p = 0; p < kPaths; ++p)
                {
                    std::vector<double> hand(n_params, 0.0), got(n_params, 0.0);
                    mc::AdjointScratch scratch{{trail.data(), 1, 0}, {hand.data(), 1}};
                    const double a = mc::script_lv_adjoint_path(v, draws, static_cast<uint64_t>(p), 1.0, scratch);
                    mc::PathDraws d = draws;
                    d.begin(static_cast<uint64_t>(p));
                    const double b =
                        ir::run_path_adjoint(primal, adj, data, c.dm, c.product.ir(), d, 1.0, 1.0, w, got.data());
                    ASSERT_TRUE(same_bits(a, b)) << what << ", path " << p;
                    expect_rows_near(got, hand, 1e-11, what + ", path " + std::to_string(p));
                }
                ++cases;
            }
        EXPECT_GT(cases, 10);
    }

    // What the transformations leave out: a tangent of a constant is not
    // computed, an adjoint block has no code for what it does not need.
    TEST(ScriptIrAdjoint, DerivativeBlocksStaySmall)
    {
        const Case c("2027-06-01\n    pays max(spot() - 100, 0)\n", Dyn::BlackScholes, ScriptSettings{});
        const ir::PathProgram primal = ir::path_program(c.product.ir(), c.dm.kind, 1, true);
        const ir::PathProgram tan = ir::tangent(primal, {0}); // delta alone
        const ir::PathProgram adj = ir::adjoint(primal);
        EXPECT_LT(tan.step.code.size(), 2 * primal.step.code.size() + 4) << ir::to_string(tan.step);
        EXPECT_LT(adj.step.code.size(), 4 * primal.step.code.size()) << ir::to_string(adj.step);
        // Delta's direction does not go through the rate: the step's tangent
        // multiplies the spot's tangent by the same exponential, no more.
        EXPECT_EQ(ir::to_string(tan.step).find("log"), std::string::npos);
    }

} // namespace quantModeling
