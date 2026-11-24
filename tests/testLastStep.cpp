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
#include "quantModeling/models/equity/rough_bergomi_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/script_writer.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <cstdlib>
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

    // A strike the script computes (issue #363): any expression, written
    // back as it was.
    TEST(LastStep, AComputedStrikeParsesAndWritesBack)
    {
        const auto events = scripting::parse_script(
            "2025-06-03\n    x = lastStepDigital(1.05 * s0) + lastStepCall(k, 1) + lastStepPut(100 + k)\n");
        const std::string text = scripting::ScriptWriter::write(events);
        EXPECT_NE(text.find("lastStepDigital((1.05 * s0))"), std::string::npos) << text;
        EXPECT_NE(text.find("lastStepCall(k, 1)"), std::string::npos) << text;
        EXPECT_NE(text.find("lastStepPut((100.0 + k))"), std::string::npos) << text;
        EXPECT_EQ(scripting::ScriptWriter::write(scripting::parse_script(text)), text);
    }

    TEST(LastStep, RejectsANonPositiveStrike)
    {
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
                                "2025-06-03\n    pays a * lastStepCall(100) + lastStepPut(90) + survivalAbove(70) + "
                                "lastStepDigital(1.1 * x) + lastStepCall(0.9 * x)\n";
        ScriptSettings tree;
        tree.bytecode = false;
        ScriptedProduct<Real> by_tree(src, kCtx, tree), by_code(src, kCtx);
        ASSERT_TRUE(by_code.program().has_monitor);
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

    // A computed strike that happens to be a constant is the literal one:
    // Black's formula at the payoff (last_step_black.hpp) against the
    // model's (last_step.hpp), on the same paths.
    TEST(LastStep, AComputedStrikeIsTheLiteralOne)
    {
        for (const char *kind : {"lastStepCall", "lastStepPut", "lastStepDigital"})
        {
            const std::string k = std::string(kind);
            const auto literal = price_lv(kMid + "2025-06-03\n    pays " + k + "(100)\n");
            const auto computed = price_lv(kMid + "    k = 100\n2025-06-03\n    pays " + k + "(k)\n");
            EXPECT_NEAR(computed.npv(), literal.npv(), 1e-12 * std::max(1.0, std::abs(literal.npv()))) << kind;
        }
    }

    // A forward-start digital under Black-Scholes, strike 105 % of the spot
    // on the intermediate date: the last step is the whole interval after
    // that date, so every path pays P(S_T > 1.05 S_t | S_t) = N(d2), which
    // does not depend on S_t -- the closed form, with no Monte-Carlo error.
    TEST(LastStep, AForwardStartDigitalIsTheClosedFormWithNoError)
    {
        const double tau = (365.0 - 183.0) / 365.0; // 2024-12-03 to 2025-06-03
        const double d2 = (std::log(1.0 / 1.05) + (r - q - 0.5 * vol * vol) * tau) / (vol * std::sqrt(tau));
        const double closed = 100.0 * std::exp(-r * T) * norm_cdf(d2);
        const auto res = price_bs(kMid + "2025-06-03\n    pays 100 * lastStepDigital(1.05 * x)\n", 1000);
        EXPECT_NEAR(res.npv(), closed, 1e-10 * closed);
        EXPECT_LT(res.std_error(), 1e-9);
        // and the hard payoff it replaces, within its error
        const auto hard = price_bs(kMid + "2025-06-03\n    if spot() > 1.05 * x then pays 100 endIf\n");
        EXPECT_NEAR(hard.npv(), closed, 4.0 * hard.std_error());
    }

    // The engine clones the model for each thread (and each multilevel
    // level): a clone computes the expectations too.
    TEST(LastStep, AClonedLocalVolModelComputesThemToo)
    {
        ScriptedProduct<Real> prod(kMid + "2025-06-03\n    pays lastStepCall(100) + lastStepDigital(110)\n", kCtx);
        LocalVolSimModel<Real> model(S0, r, q, kK, kT, skewed_surface(), 1.0 / 52.0);
        model.init(prod.timeline(), prod.defline());
        const auto clone = model.clone();
        std::vector<double> z(model.sim_dim());
        for (std::size_t i = 0; i < z.size(); ++i)
            z[i] = 1.3 * std::sin(0.7 * static_cast<double>(i));
        Scenario<Real> a, b;
        allocate_scenario(a, prod.defline(), 1);
        allocate_scenario(b, prod.defline(), 1);
        model.generate_path(z, a);
        clone->generate_path(z, b);
        ASSERT_EQ(a.back().monitored.size(), 2u);
        EXPECT_EQ(a.back().monitored, b.back().monitored);
        std::vector<Real> pa, pb;
        prod.payoffs(a, pa);
        prod.payoffs(b, pb);
        EXPECT_EQ(pa[0], pb[0]);
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

    // Rough Bergomi has no closed form over its last step: a last-step slot
    // is then the payoff of the simulated spot (an unbiased estimate of its
    // expectation), so the script prices as the hard payoff, to the bit --
    // the library's digital reads lastStepDigital under every model of the
    // Price page (issue #363). A bridge survival stays refused.
    TEST(LastStep, RoughBergomiReadsThePayoffItself)
    {
        const std::string at = "2025-06-03\n    ";
        for (const auto &[smooth, hard] :
             std::vector<std::pair<std::string, std::string>>{
                 {"pays 100 * lastStepDigital(100)", "if spot() > 100 then pays 100 endIf"},
                 {"pays 100 * lastStepDigital(1.05 * x)", "if spot() > 1.05 * x then pays 100 endIf"},
                 {"pays lastStepCall(0.95 * x)", "pays max(spot() - 0.95 * x, 0)"}})
        {
            ScriptedProduct<Real> a(kMid + at + smooth + "\n", kCtx), b(kMid + at + hard + "\n", kCtx);
            const ForwardVarianceCurve xi{{0.25, 1.0}, {0.04, 0.045}}; // the dated form: several dates
            RoughBergomiSimModel<Real> m1(S0, r, q, xi, 1.5, -0.8, 0.1, 365), m2 = m1;
            const auto pa = simulate<Real>(a, m1, settings(2000)), pb = simulate<Real>(b, m2, settings(2000));
            EXPECT_EQ(pa.npv(), pb.npv()) << smooth;
        }
        ScriptedProduct<Real> bridge("2025-06-03\n    pays survivalAbove(80)\n", kCtx);
        RoughBergomiSimModel<Real> m(S0, r, q, ForwardVarianceCurve{{1.0}, {0.04}}, 1.5, -0.8, 0.1, 365);
        EXPECT_THROW(m.init(bridge.timeline(), bridge.defline()), InvalidInput);
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

    // The GPU runs them in its kernels compiled from the IR (WP 27 §14.9);
    // with compilation off it leaves them to the CPU and says why. A bridge
    // survival stays on the CPU either way.
    TEST(LastStep, TheGpuTakesThemCompiledAndSaysWhyNot)
    {
        ScriptedProduct<Real> prod(kOneEvent, kCtx);
        DeviceModel dm;
        dm.kind = DeviceModel::Kind::BlackScholes;
        const char *saved = std::getenv("QM_SCRIPT_JIT");
        const std::string restore = saved ? saved : "";
        unsetenv("QM_SCRIPT_JIT");
        EXPECT_EQ(gpu::script_gpu_unsupported(prod.program(), dm, {{}}), "");
        setenv("QM_SCRIPT_JIT", "0", 1);
        const std::string why = gpu::script_gpu_unsupported(prod.program(), dm, {{}});
        EXPECT_NE(why.find("lastStep"), std::string::npos) << why;
        if (saved)
            setenv("QM_SCRIPT_JIT", restore.c_str(), 1);
        else
            unsetenv("QM_SCRIPT_JIT");

        // A bridge survival is compiled too (WP 27 §14.12), but not under a
        // stochastic variance.
        ScriptedProduct<Real> bridge("2025-06-03\n    pays survivalAbove(80) * lastStepCall(100)\n", kCtx);
        DeviceModel heston = dm;
        heston.kind = DeviceModel::Kind::Heston;
        EXPECT_NE(gpu::script_gpu_unsupported(bridge.program(), heston, {{}}).find("survivalAbove"), std::string::npos);
    }

} // namespace quantModeling
