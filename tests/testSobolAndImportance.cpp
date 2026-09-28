#include <gtest/gtest.h>

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
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/engines/mc/sobol_bridge_host.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

// Sobol + Brownian bridge for the GPU's per-path code, and generic importance
// sampling (blueprint/wp/19-gpu.md §2.2, §2.5; the issues that followed lot
// G3). Checked on the host, so in the CI:
//   - the flat, in-place Sobol point + bridge the kernels run gives the
//     generic engine's gaussians (SobolSequence + BridgedGaussians) bit for
//     bit;
//   - under Sobol, the GPU's risk code (duals, local-vol adjoint) gives the
//     CPU tape's risks on the same points;
//   - importance sampling is unbiased, divides a rare event's variance, and
//     steps aside when it does not help.

namespace quantModeling
{
    namespace
    {
        std::string script(const std::string &name)
        {
            std::ifstream in(std::filesystem::path(QM_PRODUCT_LIBRARY_DIR) / (name + ".qms"));
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

        template <class T>
        std::vector<T> skewed()
        {
            std::vector<T> g;
            for (Real k : kK)
                for (Real t : kT)
                    g.push_back(T(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t));
            return g;
        }

        std::vector<std::vector<Time>> discount_mats(const ScriptedProduct<Real> &p)
        {
            std::vector<std::vector<Time>> out;
            for (const SampleDef &d : p.defline())
                out.push_back(d.discount_mats);
            return out;
        }

        void expect_risks_near(const std::vector<Real> &got, const AADSimulResults &cpu, const std::string &what,
                               double rel)
        {
            ASSERT_EQ(got.size(), cpu.risks.size()) << what;
            double scale = 1.0;
            for (Real r : cpu.risks)
                scale = std::max(scale, std::abs(r));
            for (std::size_t i = 0; i < got.size(); ++i)
                EXPECT_NEAR(got[i], cpu.risks[i], rel * scale) << what << " " << cpu.risk_labels[i];
        }
    } // namespace

    // ── the flat Sobol point + bridge is the generic engine's ─────────────
    TEST(SobolBridge, FlatPointEqualsTheGenericEngines)
    {
        struct Case
        {
            std::vector<Time> times;
            std::size_t factors, stride;
        };
        const Case cases[] = {{{0.1, 0.2, 0.35, 0.5, 0.8, 1.0, 1.4}, 1, 1}, // one factor
                              {{0.25, 0.5, 0.75, 1.0}, 2, 4}};              // Bates-like: 2 factors, 2 extra draws
        for (const Case &c : cases)
        {
            const std::size_t dim = c.times.size() * c.stride;
            BrownianLayout layout{c.times, c.factors, c.stride};
            const mc::SobolTables t = mc::sobol_tables(dim, 42, 3, layout, true);
            ASSERT_TRUE(t.bridged());
            const mc::SobolBridgeView v = t.view();
            std::vector<double> pt(dim), out(dim), point(dim), ref(dim);
            for (int b = 0; b < 3; ++b)
            {
                SobolSequence seq(static_cast<int>(dim), (static_cast<uint64_t>(42u) << 32) | static_cast<uint64_t>(b));
                BridgedGaussians bridge(c.times, c.factors, c.stride);
                for (uint32_t p = 0; p < 200; ++p)
                {
                    seq.next_gaussian(point);
                    bridge.map(point, ref);
                    mc::sobol_bridged_gaussians(v, static_cast<uint64_t>(b), p, pt.data(), 1, out.data(), 1);
                    for (std::size_t j = 0; j < dim; ++j)
                        ASSERT_EQ(out[j], ref[j]) << "replicate " << b << " point " << p << " draw " << j;
                }
            }
        }
    }

    // ── Sobol under AAD: the GPU's per-path risk code = the tape ──────────
    namespace
    {
        constexpr std::uint64_t kSeed = 13;
        constexpr std::size_t kPaths = 16 * 128; // kAadRqmcReplicates x 128

        /// The GPU engines' Sobol draws on the host: replicate b, point p.
        struct HostSobol
        {
            mc::SobolTables tables;
            std::vector<double> pt, out;
            HostSobol(const ISimulationModel<Real> &m)
                : tables(mc::sobol_tables(m.sim_dim(), kSeed, kAadRqmcReplicates, m.brownian_layout(), true)),
                  pt(m.sim_dim()),
                  out(m.sim_dim())
            {
            }
            mc::PathDraws draws(int stride, uint64_t b, uint32_t p)
            {
                mc::sobol_bridged_gaussians(tables.view(), b, p, pt.data(), 1, out.data(), 1);
                mc::PathDraws d;
                d.stride = stride;
                d.given = out.data();
                d.begin(b * (kPaths / kAadRqmcReplicates) + p);
                return d;
            }
        };
    } // namespace

    TEST(SobolRisks, DualsMatchTheTapeUnderSobol)
    {
        for (const std::string name : {"european-call", "asian-call", "phoenix-autocall", "up-and-out-call"})
        {
            const ScriptedProduct<Real> product(script(name), kCtx);
            BatesSimModel<Real> model(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0, 0.0, 0.0, 1.0 / 52.0);
            model.init(product.timeline(), product.defline());
            DeviceModel dm;
            ASSERT_TRUE(model.describe_device(dm));
            const auto mats = discount_mats(product);
            const std::vector<Real> base = product.baseline();
            const mc::ScriptPathHost host(product.program(), dm, mats, base);
            HostSobol sobol(model);
            const auto in = mc::script_dual_inputs<8>(host.view);
            const std::size_t per = kPaths / kAadRqmcReplicates;
            std::vector<Real> risks(model.parameter_labels().size(), 0.0);
            double price = 0.0;
            for (int b = 0; b < kAadRqmcReplicates; ++b)
                for (uint32_t p = 0; p < per; ++p)
                {
                    mc::PathDraws d = sobol.draws(host.view.stride, static_cast<uint64_t>(b), p);
                    const Dual<8> x = mc::script_path(host.view, in, d, 1.0, nullptr);
                    price += x.v / kPaths;
                    for (int i = 0; i < 8; ++i)
                        risks[static_cast<std::size_t>(i)] += x.d[i] / kPaths;
                }

            const ScriptedProduct<aad::Number> tape_product(script(name), kCtx);
            BatesSimModel<aad::Number> tape_model(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01),
                                                  aad::Number(0.04), aad::Number(1.5), aad::Number(0.05),
                                                  aad::Number(0.6), aad::Number(-0.7), aad::Number(0.0),
                                                  aad::Number(0.0), aad::Number(0.0), 1.0 / 52.0);
            const AADSimulResults cpu = simulate_aad(tape_product, tape_model, kPaths, kSeed, first_aad_payoff,
                                                     RngKind::Philox, SamplerKind::Sobol);
            EXPECT_NE(cpu.diagnostics.find("Sobol"), std::string::npos) << cpu.diagnostics;
            EXPECT_NEAR(price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << name;
            expect_risks_near(risks, cpu, name, 1e-9);
        }
    }

    TEST(SobolRisks, LocalVolAdjointMatchesTheTapeUnderSobol)
    {
        for (const std::string name : {"european-call", "asian-call", "up-and-out-call", "variance-swap"})
        {
            const ScriptedProduct<Real> product(script(name), kCtx);
            LocalVolSimModel<Real> model(100.0, 0.03, 0.01, kK, kT, skewed<Real>(), 1.0 / 24.0);
            model.init(product.timeline(), product.defline());
            DeviceModel dm;
            ASSERT_TRUE(model.describe_device(dm));
            const auto mats = discount_mats(product);
            const std::vector<Real> base = product.baseline();
            const mc::ScriptPathHost host(product.program(), dm, mats, base);
            HostSobol sobol(model);

            std::vector<double> trail(static_cast<std::size_t>(mc::lv_trail_bound(product.program(), host.view.n_steps)));
            std::vector<double> grad(3 + kK.size() * kT.size(), 0.0);
            mc::AdjointScratch w{{trail.data(), 1, 0}, {grad.data(), 1}};
            const std::size_t per = kPaths / kAadRqmcReplicates;
            double price = 0.0;
            for (int b = 0; b < kAadRqmcReplicates; ++b)
                for (uint32_t p = 0; p < per; ++p)
                {
                    const mc::PathDraws d = sobol.draws(1, static_cast<uint64_t>(b), p);
                    price += mc::script_lv_adjoint_path(host.view, d, b * per + p, 1.0, w) / kPaths;
                }
            for (double &g : grad)
                g /= static_cast<double>(kPaths);

            const ScriptedProduct<aad::Number> tape_product(script(name), kCtx);
            LocalVolSimModel<aad::Number> tape_model(aad::Number(100.0), aad::Number(0.03), aad::Number(0.01), kK,
                                                     kT, skewed<aad::Number>(), 1.0 / 24.0);
            const AADSimulResults cpu = simulate_aad(tape_product, tape_model, kPaths, kSeed, first_aad_payoff,
                                                     RngKind::Philox, SamplerKind::Sobol);
            EXPECT_NEAR(price, cpu.price, 1e-10 * std::max(1.0, std::abs(cpu.price))) << name;
            expect_risks_near(grad, cpu, name, 1e-8);
        }
    }

    // ── importance sampling ────────────────────────────────────────────────
    namespace
    {
        /// A one-year digital paying 100 above 180: ~0.8 % likely.
        const std::string kDeepDigital = "2027-06-01\n    if spot() > 180 then pays 100 endIf\n";

        double digital_bs(double S, double K, double r, double q, double sigma, double T)
        {
            const double d2 = (std::log(S / K) + (r - q - 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
            return 100.0 * std::exp(-r * T) * norm_cdf(d2);
        }

        PricingSettings is_settings(bool is, SamplerKind sampler = SamplerKind::PseudoRandom)
        {
            PricingSettings s;
            s.mc_paths = 1 << 16;
            s.mc_seed = 3;
            s.mc_rng = RngKind::Philox;
            s.mc_sampler = sampler;
            s.mc_importance_drift = is;
            return s;
        }
    } // namespace

    TEST(ImportanceSampling, RareDigitalIsUnbiasedAndFarTighter)
    {
        const ScriptedProduct<Real> product(kDeepDigital, kCtx);
        const double T = kCtx.t(Date::from_iso("2027-06-01"));
        const double exact = digital_bs(100.0, 180.0, 0.03, 0.01, 0.25, T);

        BlackScholesSimModel<Real> m1(100.0, 0.03, 0.01, 0.25), m2(100.0, 0.03, 0.01, 0.25);
        const SimulationMCResult plain = simulate<Real>(product, m1, is_settings(false));
        const SimulationMCResult is = simulate<Real>(product, m2, is_settings(true));
        ASSERT_NE(is.diagnostics.find("+ importance sampling (drift"), std::string::npos) << is.diagnostics;
        EXPECT_NEAR(plain.npv(), exact, 4.0 * plain.std_error());
        EXPECT_NEAR(is.npv(), exact, 4.0 * is.std_error()) << is.diagnostics;
        const double ratio = (plain.std_error() * plain.std_error()) / (is.std_error() * is.std_error());
        EXPECT_GT(ratio, 10.0) << is.diagnostics;
    }

    // IS is a change of variable of the integrand: it composes with the
    // other samplers and with the controls without bias.
    TEST(ImportanceSampling, ComposesWithTheOtherSamplers)
    {
        const ScriptedProduct<Real> product(kDeepDigital, kCtx);
        const double T = kCtx.t(Date::from_iso("2027-06-01"));
        const double exact = digital_bs(100.0, 180.0, 0.03, 0.01, 0.25, T);
        for (const SamplerKind sampler : {SamplerKind::Stratified, SamplerKind::Sobol})
            for (const bool control : {false, true})
            {
                PricingSettings s = is_settings(true, sampler);
                s.mc_spot_control = control;
                LocalVolSimModel<Real> m(100.0, 0.03, 0.01, {60, 100, 140, 180, 220}, {0.5, 1.0, 2.0},
                                         std::vector<Real>(15, 0.25), 1.0 / 12.0);
                const SimulationMCResult r = simulate<Real>(product, m, s);
                ASSERT_NE(r.diagnostics.find("+ importance sampling"), std::string::npos) << r.diagnostics;
                // flat local vol = Black-Scholes, up to the Euler grid (exact in mean for log-Euler)
                EXPECT_NEAR(r.npv(), exact, 4.0 * r.std_error() + 2e-3) << r.diagnostics;
            }
    }

    // A payoff whose mode is at zero drift, or where the drift does not help:
    // the pilot catches it and the run says so.
    TEST(ImportanceSampling, StepsAsideWhenItDoesNotHelp)
    {
        const ScriptedProduct<Real> product(script("phoenix-autocall"), kCtx);
        BlackScholesSimModel<Real> m(100.0, 0.03, 0.01, 0.25);
        const SimulationMCResult r = simulate<Real>(product, m, is_settings(true));
        const bool kept = r.diagnostics.find("+ importance sampling (drift") != std::string::npos;
        const bool declined = r.diagnostics.find("(no importance sampling:") != std::string::npos;
        EXPECT_TRUE(kept != declined) << r.diagnostics;
        BlackScholesSimModel<Real> m2(100.0, 0.03, 0.01, 0.25);
        const SimulationMCResult plain = simulate<Real>(product, m2, is_settings(false));
        EXPECT_NEAR(r.npv(), plain.npv(), 4.0 * std::hypot(r.std_error(), plain.std_error()));
    }

} // namespace quantModeling
