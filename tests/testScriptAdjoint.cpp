#include <gtest/gtest.h>

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_adjoint.hpp"
#include "quantModeling/engines/mc/script_path_host.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"

// Lot G3 of blueprint/wp/19-gpu.md §6: the GPU's model risks, computed by
// the per-path code the kernels run -- here on the host, so that the CI
// (without a GPU) checks them against the CPU tape, script by script:
//   - the bytecode's adjoint, instruction by instruction;
//   - the local-vol per-path adjoint (spot, rate, div and every sigma_loc
//     point), the superbucket's input;
//   - forward-mode duals under Black-Scholes, Heston and two assets.
// Same Philox draws on both sides, so the risks agree up to rounding.

namespace quantModeling
{
    namespace
    {
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

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

        std::vector<Real> skewed()
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
            return g;
        }

        std::vector<std::vector<Time>> discount_mats(const ScriptedProduct<Real> &p)
        {
            std::vector<std::vector<Time>> out;
            for (const SampleDef &d : p.defline())
                out.push_back(d.discount_mats);
            return out;
        }

        std::vector<Real> baseline(const ScriptedProduct<Real> &p)
        {
            return p.baseline();
        }

        void expect_risks_near(const std::vector<Real> &got, const AADSimulResults &cpu, const std::string &what,
                               double rel = 1e-8)
        {
            ASSERT_EQ(got.size(), cpu.risks.size()) << what;
            double scale = 1.0;
            for (Real r : cpu.risks)
                scale = std::max(scale, std::abs(r));
            for (std::size_t i = 0; i < got.size(); ++i)
                EXPECT_NEAR(got[i], cpu.risks[i], rel * scale) << what << " " << cpu.risk_labels[i];
        }

        constexpr std::uint64_t kSeed = 19;
        constexpr std::size_t kPaths = 256; // a multiple of simulate_aad's batch of 64
    } // namespace

    // ── the bytecode's adjoint against the tape, event by event ───────────
    TEST(ScriptAdjoint, BytecodeAdjointMatchesTheTape)
    {
        for (const bool fuzzy : {false, true})
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(s.source, kCtx, ss);
                const ScriptedProduct<aad::Number> tape_product(s.source, kCtx, ss);
                const scripting::Program &prog = product.program();
                const std::size_t n_events = prog.n_events();
                const std::size_t n_assets = product.n_underlyings();

                // A deterministic, wiggly scenario: every spot, df and
                // numeraire different, around the script's thresholds.
                std::vector<std::vector<double>> spots(n_events, std::vector<double>(n_assets));
                std::vector<std::vector<double>> discs(n_events);
                std::vector<double> num(n_events);
                for (std::size_t e = 0; e < n_events; ++e)
                {
                    for (std::size_t a = 0; a < n_assets; ++a)
                        spots[e][a] = 100.0 * (1.0 + 0.23 * std::sin(1.7 * static_cast<double>(e) +
                                                                     0.9 * static_cast<double>(a) + 0.3));
                    discs[e].assign(product.defline()[e].discount_mats.size(), 0.97);
                    for (std::size_t i = 0; i < discs[e].size(); ++i)
                        discs[e][i] = 0.97 - 0.01 * static_cast<double>(i);
                    num[e] = 1.0 + 0.01 * static_cast<double>(e);
                }

                // Tape.
                aad::Number::tape->rewind();
                Scenario<aad::Number> path;
                allocate_scenario(path, tape_product.defline(), n_assets);
                for (std::size_t e = 0; e < n_events; ++e)
                {
                    for (std::size_t a = 0; a < n_assets; ++a)
                        path[e].spots[a] = aad::Number(spots[e][a]);
                    for (std::size_t i = 0; i < discs[e].size(); ++i)
                        path[e].discounts[i] = aad::Number(discs[e][i]);
                    path[e].numeraire = aad::Number(num[e]);
                }
                std::vector<aad::Number> pay;
                tape_product.payoffs(path, pay);
                pay[0].propagate_to_start();

                // Trail.
                std::vector<double> buf(static_cast<std::size_t>(mc::lv_trail_bound(prog, 0)) + 16);
                scripting::Trail trail{buf.data(), 1, 0};
                std::vector<double> vars(static_cast<std::size_t>(std::max(prog.n_vars, 1))),
                    stack(static_cast<std::size_t>(std::max(prog.max_stack, 1))),
                    degrees(static_cast<std::size_t>(std::max(prog.max_degrees, 1))),
                    slots(static_cast<std::size_t>(std::max(prog.n_if_slots, 1)));
                std::vector<int> modes(static_cast<std::size_t>(std::max(prog.n_if_modes, 1)));
                const std::vector<Real> base = product.baseline();
                for (std::size_t i = 0; i < vars.size() && i < static_cast<std::size_t>(prog.n_vars); ++i)
                    vars[i] = base.empty() ? 0.0 : base[i];
                scripting::Machine<double> m{vars.data(), stack.data(), degrees.data(), slots.data(),
                                             modes.data(), 0.0};
                for (std::size_t e = 0; e < n_events; ++e)
                    scripting::run_event_recorded(prog.view(), prog.event_begin[e], prog.event_begin[e + 1], m,
                                                  spots[e].data(), discs[e].data(), num[e], trail);
                ASSERT_NEAR(m.payoff, pay[0].value(), 1e-12 * std::max(1.0, std::abs(m.payoff))) << s.name;

                std::vector<double> a_vars(vars.size(), 0.0), a_stack(stack.size(), 0.0),
                    a_deg(degrees.size(), 0.0), a_slots(slots.size(), 0.0);
                scripting::AdjointMachine adj{a_vars.data(), a_stack.data(), a_deg.data(), a_slots.data(), 1.0};
                for (std::size_t e = n_events; e-- > 0;)
                {
                    std::vector<double> a_spots(n_assets, 0.0), a_disc(discs[e].size() + 1, 0.0);
                    double a_num = 0.0;
                    scripting::reverse_event(prog.view(), adj, a_spots.data(), a_disc.data(), a_num, trail);
                    const std::string at = s.name + (fuzzy ? " fuzzy" : " hard") + " event " + std::to_string(e);
                    for (std::size_t a = 0; a < n_assets; ++a)
                        EXPECT_NEAR(a_spots[a], path[e].spots[a].adjoint(),
                                    1e-10 * std::max(1.0, std::abs(a_spots[a])))
                            << at << " spot " << a;
                    for (std::size_t i = 0; i < discs[e].size(); ++i)
                        EXPECT_NEAR(a_disc[i], path[e].discounts[i].adjoint(), 1e-9) << at << " df " << i;
                    EXPECT_NEAR(a_num, path[e].numeraire.adjoint(), 1e-9 * std::max(1.0, std::abs(a_num)))
                        << at << " numeraire";
                }
                EXPECT_EQ(trail.top, 0) << s.name;
            }
    }

    // ── local vol: the per-path adjoint against simulate_aad ──────────────
    TEST(ScriptAdjoint, LocalVolAdjointMatchesTheTape)
    {
        for (const bool fuzzy : {false, true})
            for (const auto &s : library())
            {
                ScriptSettings ss;
                ss.fuzzy = fuzzy;
                const ScriptedProduct<Real> product(s.source, kCtx, ss);
                if (product.n_underlyings() > 1)
                    continue;
                LocalVolSimModel<Real> model(100.0, 0.03, 0.01, kK, kT, skewed(), 1.0 / 24.0);
                model.init(product.timeline(), product.defline());
                DeviceModel dm;
                ASSERT_TRUE(model.describe_device(dm));
                const auto mats = discount_mats(product);
                const auto base = baseline(product);
                const mc::ScriptPathHost host(product.program(), dm, mats, base);

                const std::size_t n_params = 3 + kK.size() * kT.size();
                std::vector<double> trail(static_cast<std::size_t>(mc::lv_trail_bound(product.program(),
                                                                                      host.view.n_steps)));
                std::vector<double> grad(n_params, 0.0);
                mc::AdjointScratch w{{trail.data(), 1, 0}, {grad.data(), 1}};
                mc::PathDraws draws;
                draws.seed = kSeed;
                double price = 0.0;
                for (std::size_t p = 0; p < kPaths; ++p)
                    price += mc::script_lv_adjoint_path(host.view, draws, p, 1.0, w);
                price /= static_cast<double>(kPaths);
                for (double &g : grad)
                    g /= static_cast<double>(kPaths);

                const ScriptedProduct<aad::Number> tape_product(s.source, kCtx, ss);
                std::vector<aad::Number> sig;
                for (Real x : skewed())
                    sig.emplace_back(x);
                LocalVolSimModel<aad::Number> tape_model(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01), kK, kT, sig,
                                                         1.0 / 24.0);
                const AADSimulResults cpu = simulate_aad(tape_product, tape_model, kPaths, kSeed, first_aad_payoff,
                                                         RngKind::Philox);
                const std::string what = s.name + (fuzzy ? " fuzzy" : " hard");
                EXPECT_NEAR(price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << what;
                expect_risks_near(grad, cpu, what);
            }
    }

    // ── duals: Black-Scholes, Heston, two assets ──────────────────────────
    namespace
    {
        template <int N>
        std::vector<Real> dual_risks(const mc::ScriptPathView &v, std::size_t n_labels, double &price)
        {
            mc::PathDraws draws;
            draws.seed = kSeed;
            draws.stride = v.stride;
            const auto in = mc::script_dual_inputs<N>(v);
            std::vector<Real> out(n_labels, 0.0);
            price = 0.0;
            for (std::size_t p = 0; p < kPaths; ++p)
            {
                draws.begin(p);
                const Dual<N> x = mc::script_path(v, in, draws, 1.0, nullptr);
                price += x.v;
                for (int i = 0; i < N && static_cast<std::size_t>(i) < n_labels; ++i)
                    out[static_cast<std::size_t>(i)] += x.d[i];
            }
            price /= static_cast<double>(kPaths);
            for (Real &r : out)
                r /= static_cast<double>(kPaths);
            return out;
        }

        template <class MakeModel>
        void expect_duals_match(bool multi_asset, MakeModel make)
        {
            for (const auto &s : library())
            {
                const ScriptedProduct<Real> product(s.source, kCtx);
                if ((product.n_underlyings() > 1) != multi_asset)
                    continue;
                if (multi_asset && product.n_underlyings() != 2)
                    continue;
                auto model = make(Real{}, product.n_underlyings());
                model->init(product.timeline(), product.defline());
                DeviceModel dm;
                ASSERT_TRUE(model->describe_device(dm)) << s.name;
                const auto mats = discount_mats(product);
                const auto base = baseline(product);
                const mc::ScriptPathHost host(product.program(), dm, mats, base);

                const ScriptedProduct<aad::Number> tape_product(s.source, kCtx);
                auto tape_model = make(aad::Number{}, product.n_underlyings());
                const AADSimulResults cpu = simulate_aad(tape_product, *tape_model, kPaths, kSeed, first_aad_payoff,
                                                         RngKind::Philox);
                const int dirs = mc::dual_directions(dm.kind, dm.n_assets);
                ASSERT_GT(dirs, 0);
                double price = 0.0;
                const std::vector<Real> got = dual_risks<8>(host.view, cpu.risks.size(), price);
                EXPECT_NEAR(price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << s.name;
                expect_risks_near(got, cpu, s.name, 1e-9);
            }
        }
    } // namespace

    TEST(ScriptDuals, BlackScholesMatchesTheTape)
    {
        expect_duals_match(false, [](auto zero, std::size_t)
                           {
            using T = decltype(zero);
            return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25)); });
    }

    TEST(ScriptDuals, HestonMatchesTheTape)
    {
        expect_duals_match(false, [](auto zero, std::size_t)
                           {
            using T = decltype(zero);
            return std::make_unique<BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05), T(0.6),
                                                      T(-0.7), T(0.0), T(0.0), T(0.0)); });
    }

    TEST(ScriptDuals, TwoAssetsMatchTheTape)
    {
        expect_duals_match(true, [](auto zero, std::size_t n)
                           {
            using T = decltype(zero);
            Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n),
                                                             static_cast<Eigen::Index>(n), 0.4);
            corr.diagonal().setOnes();
            std::vector<T> s0, q, vol;
            for (std::size_t i = 0; i < n; ++i)
            {
                s0.push_back(T(100.0 - 4.0 * static_cast<double>(i)));
                q.push_back(T(0.01));
                vol.push_back(T(0.2 + 0.04 * static_cast<double>(i)));
            }
            return std::make_unique<MultiAssetBSSimModel<T>>(s0, T(0.03), q, vol, corr); });
    }

} // namespace quantModeling
