// The last-step expectations (WP 27 §8, rung 1): lastStepCall(K),
// lastStepPut(K) and lastStepDigital(K) return the expectation of the payoff
// at the current event over the model's last step, given the path before it
// -- Black's formula on the step's lognormal law.
//
// Properties checked:
//  - parsing, writing back and the language's errors;
//  - Black-Scholes, one event: the last step is the whole path, so the
//    script is the closed form, with no Monte-Carlo error at all;
//  - with an intermediate event: call, put and digital reprice their closed
//    forms within error, with a smaller error than the payoffs they replace;
//  - local vol on a skewed surface: the same price as the payoff itself
//    (no bias against the scheme), and put-call parity;
//  - tree evaluator = bytecode = batched bytecode, to the bit;
//  - the acceptance of the rung: gamma, vanna and volga by adjoint over
//    tangent on the tape equal Black-Scholes's, where the payoff max(S - K, 0)
//    gives a pathwise gamma of exactly 0;
//  - a model that does not compute it, the Milstein step and the GPU refuse
//    it and say why.

#include <gtest/gtest.h>

#include "quantModeling/aad/tangent.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad2.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/merton_jump_diffusion_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/script_writer.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr double S0 = 100.0, K = 100.0, r = 0.03, q = 0.01, vol = 0.25, T = 1.0;

        const ValuationContext kCtx{Date::from_iso("2024-06-03")};
        // 2025-06-03 is 365 days later: T = 1 under ACT/365F.
        const std::string kOneEvent = "2025-06-03\n    pays lastStepCall(100)\n";
        const std::string kMid = "2024-12-03\n    x = spot()\n";

        PricingSettings settings(int paths = 100000, SamplerKind sampler = SamplerKind::Sobol)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 20241203;
            s.mc_sampler = sampler;
            return s;
        }

        double d1()
        {
            return (std::log(S0 / K) + (r - q + 0.5 * vol * vol) * T) / (vol * std::sqrt(T));
        }
        double d2()
        {
            return d1() - vol * std::sqrt(T);
        }
        double bs_call()
        {
            return S0 * std::exp(-q * T) * norm_cdf(d1()) - K * std::exp(-r * T) * norm_cdf(d2());
        }
        double bs_put()
        {
            return K * std::exp(-r * T) * norm_cdf(-d2()) - S0 * std::exp(-q * T) * norm_cdf(-d1());
        }
        double bs_digital()
        {
            return std::exp(-r * T) * norm_cdf(d2());
        }

        SimulationMCResult price_bs(const std::string &script, int paths = 100000,
                                    SamplerKind sampler = SamplerKind::Sobol)
        {
            ScriptedProduct<Real> prod(script, kCtx);
            BlackScholesSimModel<Real> model(S0, r, q, vol);
            return simulate<Real>(prod, model, settings(paths, sampler));
        }

        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};
        std::vector<Real> skewed_surface()
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (std::size_t j = 0; j < kT.size(); ++j)
                    g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0);
            return g;
        }

        SimulationMCResult price_lv(const std::string &script)
        {
            ScriptedProduct<Real> prod(script, kCtx);
            LocalVolSimModel<Real> model(S0, r, q, kK, kT, skewed_surface(), 1.0 / 52.0);
            return simulate<Real>(prod, model, settings());
        }
    } // namespace

    TEST(LastStep, ParsesAndWritesBack)
    {
        const auto events = scripting::parse_script(
            "2025-06-03\n    x = lastStepCall(100) + lastStepPut(95.5, 1) * lastStepDigital(110)\n");
        const std::string text = scripting::ScriptWriter::write(events);
        EXPECT_NE(text.find("lastStepCall(100.0)"), std::string::npos) << text;
        EXPECT_NE(text.find("lastStepPut(95.5, 1)"), std::string::npos) << text;
        EXPECT_NE(text.find("lastStepDigital(110.0)"), std::string::npos) << text;
        EXPECT_EQ(scripting::ScriptWriter::write(scripting::parse_script(text)), text);
    }

    TEST(LastStep, RejectsANonLiteralOrNonPositiveStrike)
    {
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = lastStepCall(k)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = lastStepPut(0)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = lastStepDigital(100, 1.5)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = lastStepCall()\n"));
    }

    // One event: the last step is the whole path from the valuation date, so
    // every path pays the closed form.
    TEST(LastStep, OneEventUnderBlackScholesIsTheClosedFormWithNoError)
    {
        const auto res = price_bs(kOneEvent, 1000);
        EXPECT_NEAR(res.npv(), bs_call(), 1e-10 * bs_call());
        EXPECT_LT(res.std_error(), 1e-10);
    }

    TEST(LastStep, CallPutAndDigitalRepriceTheirClosedForms)
    {
        const auto call = price_bs(kMid + "2025-06-03\n    pays lastStepCall(100)\n");
        const auto put = price_bs(kMid + "2025-06-03\n    pays lastStepPut(100)\n");
        const auto digital = price_bs(kMid + "2025-06-03\n    pays lastStepDigital(100)\n");
        EXPECT_NEAR(call.npv(), bs_call(), 4.0 * call.std_error()) << "closed form " << bs_call();
        EXPECT_NEAR(put.npv(), bs_put(), 4.0 * put.std_error()) << "closed form " << bs_put();
        EXPECT_NEAR(digital.npv(), bs_digital(), 4.0 * digital.std_error()) << "closed form " << bs_digital();

        // The last step (half the variance here) is integrated out: a
        // smaller error than the payoffs they replace, on the same
        // pseudo-random paths (Sobol's replicate errors do not compare as
        // variances do).
        const auto pr = SamplerKind::PseudoRandom;
        const auto smooth_call = price_bs(kMid + "2025-06-03\n    pays lastStepCall(100)\n", 100000, pr);
        const auto smooth_digital = price_bs(kMid + "2025-06-03\n    pays lastStepDigital(100)\n", 100000, pr);
        const auto plain_call = price_bs(kMid + "2025-06-03\n    pays max(spot() - 100, 0)\n", 100000, pr);
        const auto plain_digital =
            price_bs(kMid + "2025-06-03\n    if spot() > 100 then pays 1 endIf\n", 100000, pr);
        EXPECT_LT(smooth_call.std_error(), 0.8 * plain_call.std_error());
        EXPECT_LT(smooth_digital.std_error(), 0.8 * plain_digital.std_error());
    }

    // Local vol: the expectation over the scheme's own last step, so the
    // price is the payoff's (the tower property), not Black-Scholes's.
    TEST(LastStep, SkewedLocalVolHasThePayoffsPriceAndParity)
    {
        const std::string tail = "2025-06-03\n    pays ";
        const auto call = price_lv(kMid + tail + "lastStepCall(100)\n");
        const auto plain = price_lv(kMid + tail + "max(spot() - 100, 0)\n");
        EXPECT_NEAR(call.npv(), plain.npv(), 4.0 * std::hypot(call.std_error(), plain.std_error()))
            << "last step " << call.npv() << ", payoff " << plain.npv();
        // (No claim on the error here: a weekly last step carries 2 % of the
        // variance, so there is nearly nothing to integrate out.)

        // Call - put = E[S_T] e^{-rT} - K e^{-rT}, path by path in
        // expectation: the forward under the scheme.
        const auto put = price_lv(kMid + tail + "lastStepPut(100)\n");
        const auto forward = price_lv(kMid + tail + "spot() - 100\n");
        EXPECT_NEAR(call.npv() - put.npv(), forward.npv(),
                    4.0 * std::hypot(std::hypot(call.std_error(), put.std_error()), forward.std_error()));
    }

    TEST(LastStep, TreeBytecodeAndBatchAgreeToTheBit)
    {
        const std::string src = "2024-09-03\n    a = lastStepDigital(105)\n" + kMid +
                                "2025-06-03\n    pays a * lastStepCall(100) + lastStepPut(90) + survivalAbove(70)\n";
        ScriptSettings tree;
        tree.bytecode = false;
        ScriptedProduct<Real> by_tree(src, kCtx, tree), by_code(src, kCtx);
        ASSERT_TRUE(by_code.program().has_survival);
        ASSERT_TRUE(by_code.program().batchable());

        LocalVolSimModel<Real> model(S0, r, q, kK, kT, skewed_surface(), 1.0 / 52.0);
        model.init(by_code.timeline(), by_code.defline());
        constexpr int W = scripting::kBatch;
        std::vector<Scenario<Real>> paths(W);
        std::vector<const Scenario<Real> *> ptrs;
        for (int p = 0; p < W; ++p)
        {
            allocate_scenario(paths[p], by_code.defline(), 1);
            std::vector<double> z(model.sim_dim());
            for (std::size_t i = 0; i < z.size(); ++i)
                z[i] = 1.3 * std::sin(0.7 * static_cast<double>(i) + 1.1 * p);
            model.generate_path(z, paths[p]);
            ptrs.push_back(&paths[p]);
        }
        auto state = by_code.make_batch_state();
        ASSERT_NE(state, nullptr);
        std::vector<Real> batch(W);
        by_code.payoffs_batch(ptrs.data(), W, batch.data(), *state);
        for (int p = 0; p < W; ++p)
        {
            std::vector<Real> a, b;
            by_tree.payoffs(paths[p], a);
            by_code.payoffs(paths[p], b);
            EXPECT_EQ(a[0], b[0]) << "path " << p;
            EXPECT_EQ(b[0], batch[p]) << "path " << p;
        }
    }

    namespace
    {
        using N2 = SecondOrderNumber;
        constexpr std::size_t kSpot = 0, kVol = 3; // BlackScholesSimModel: spot, rate, div, vol

        AADSecondOrderResults second_order(const std::string &script, std::size_t direction, std::size_t paths)
        {
            ScriptedProduct<N2> product(script, kCtx);
            BlackScholesSimModel<N2> model{N2(S0), N2(r), N2(q), N2(vol)};
            return simulate_aad_second_order(product, model, direction, paths, 11);
        }
    } // namespace

    // The acceptance of rung 1 (WP 27 §8, §10 L6): the call's pathwise
    // second-order risks by adjoint over tangent are Black-Scholes's, with
    // no smoothing bias. The payoff max(S - K, 0) on the same paths has a
    // pathwise gamma of exactly 0.
    TEST(LastStep, SecondOrderRisksOnTheTapeAreBlackScholess)
    {
        const std::string smooth = kMid + "2025-06-03\n    pays lastStepCall(100)\n";
        const double n1 = norm_pdf(d1()), sqT = std::sqrt(T);
        const double gamma = std::exp(-q * T) * n1 / (S0 * vol * sqT);
        const double vanna = -std::exp(-q * T) * n1 * d2() / vol;
        const double volga = S0 * std::exp(-q * T) * n1 * sqT * d1() * d2() / vol;

        const auto by_spot = second_order(smooth, kSpot, 20000);
        EXPECT_NEAR(by_spot.second[kSpot], gamma, 4.0 * by_spot.second_std_errors[kSpot]) << "gamma";
        EXPECT_NEAR(by_spot.second[kVol], vanna, 4.0 * by_spot.second_std_errors[kVol]) << "vanna";
        EXPECT_LT(by_spot.second_std_errors[kSpot], 0.02 * gamma);
        const auto by_vol = second_order(smooth, kVol, 20000);
        EXPECT_NEAR(by_vol.second[kVol], volga, 4.0 * by_vol.second_std_errors[kVol] + 1e-3 * std::abs(volga))
            << "volga";

        const auto kink = second_order(kMid + "2025-06-03\n    pays max(spot() - 100, 0)\n", kSpot, 2000);
        EXPECT_EQ(kink.second[kSpot], 0.0);
    }

    TEST(LastStep, ModelsThatDoNotComputeItRefuseTheScript)
    {
        ScriptedProduct<Real> prod(kOneEvent, kCtx);
        MertonJumpDiffusionSimModel<Real> merton(S0, r, q, vol, 0.3, -0.05, 0.1);
        try
        {
            merton.init(prod.timeline(), prod.defline());
            FAIL() << "Merton accepted lastStepCall()";
        }
        catch (const InvalidInput &e)
        {
            EXPECT_NE(std::string(e.what()).find("lastStepCall"), std::string::npos) << e.what();
        }
    }

    TEST(LastStep, TheMilsteinStepIsRefused)
    {
        ScriptedProduct<Real> prod(kMid + "2025-06-03\n    pays lastStepCall(100)\n", kCtx);
        LocalVolSimModel<Real> model(S0, r, q, kK, kT, skewed_surface(), 1.0 / 52.0);
        model.set_milstein(true);
        EXPECT_THROW(simulate<Real>(prod, model, settings(100)), InvalidInput);
    }

    TEST(LastStep, TheGpuLeavesItToTheCpuAndSaysWhy)
    {
        ScriptedProduct<Real> prod(kOneEvent, kCtx);
        DeviceModel dm;
        dm.kind = DeviceModel::Kind::BlackScholes;
        const std::string why = gpu::script_gpu_unsupported(prod.program(), dm, {{}});
        EXPECT_NE(why.find("lastStep"), std::string::npos) << why;
    }

} // namespace quantModeling
