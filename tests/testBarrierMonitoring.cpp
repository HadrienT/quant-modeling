// Continuous barrier monitoring in scripts (WP 26 P2): survivalAbove(L) and
// survivalBelow(L) return the probability that the spot stayed on the safe
// side of L since the previous event, the product of the Brownian-bridge
// survivals over the model's steps.
//
// Properties checked:
//  - parsing, writing back and the language's errors;
//  - Black-Scholes, one event: the bridge is exact, so a down-and-out call
//    written with survivalAbove reprices the continuous-monitoring closed
//    form (Reiner-Rubinstein) within Monte-Carlo error, and in + out = vanilla;
//  - the same price whatever the number of intermediate events (the
//    survival of a union of intervals is the product of theirs);
//  - local vol on a flat surface is Black-Scholes; on a skewed surface the
//    script matches the hand-written barrier engine (bridge on), the oracle
//    P3 will retire;
//  - tree evaluator = bytecode = batched bytecode, to the bit;
//  - a model that does not monitor barriers refuses the script, and the GPU
//    says why it leaves it to the CPU.

#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/merton_jump_diffusion_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/registry.hpp"
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
        constexpr double S0 = 100.0, K = 100.0, r = 0.05, q = 0.02, vol = 0.2;

        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2024-06-03")};
        }

        PricingSettings settings(int paths = 200000, int seed = 20240603)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = seed;
            return s;
        }

        double bs_call(double S, double k, double T)
        {
            const double sd = vol * std::sqrt(T);
            const double d1 = (std::log(S / k) + (r - q + 0.5 * vol * vol) * T) / sd;
            return S * std::exp(-q * T) * norm_cdf(d1) - k * std::exp(-r * T) * norm_cdf(d1 - sd);
        }

        // Down-and-out call, H <= K, continuous monitoring, no rebate
        // (Reiner & Rubinstein 1991; Hull, Options, Futures and Other
        // Derivatives, §26.9): C - C_di with
        // C_di = S e^{-qT} (H/S)^{2λ} N(y) - K e^{-rT} (H/S)^{2λ-2} N(y - σ√T).
        double down_and_out_call(double H, double T)
        {
            const double sd = vol * std::sqrt(T);
            const double lambda = (r - q + 0.5 * vol * vol) / (vol * vol);
            const double y = std::log(H * H / (S0 * K)) / sd + lambda * sd;
            const double di = S0 * std::exp(-q * T) * std::pow(H / S0, 2.0 * lambda) * norm_cdf(y) -
                              K * std::exp(-r * T) * std::pow(H / S0, 2.0 * lambda - 2.0) * norm_cdf(y - sd);
            return bs_call(S0, K, T) - di;
        }

        const std::string kOneEvent = "2025-06-03\n    pays survivalAbove(85) * max(spot() - 100, 0)\n";

        // The same option with monthly intermediate events: each event's
        // survival covers the month before it.
        const std::string kMonthly = "2024-07-03 2024-08-03 2024-09-03 2024-10-03 2024-11-03 2024-12-03 "
                                     "2025-01-03 2025-02-03 2025-03-03 2025-04-03 2025-05-03\n"
                                     "    alive = alive * survivalAbove(85)\n"
                                     "2025-06-03\n"
                                     "    pays alive * survivalAbove(85) * max(spot() - 100, 0)\n";

        const std::string kPreamble = "2024-06-04\n    alive = survivalAbove(85)\n";

        const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
        const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

        std::vector<Real> surface(bool skewed)
        {
            std::vector<Real> g;
            for (Real k : kK)
                for (std::size_t j = 0; j < kT.size(); ++j)
                    g.push_back(skewed ? 0.22 + 0.3 * (100.0 - k) / 100.0 : vol);
            return g;
        }
    } // namespace

    TEST(BarrierMonitoring, ParsesAndWritesBack)
    {
        const auto events = scripting::parse_script("2025-06-03\n    x = survivalAbove(85) * survivalBelow(120.5, 1)\n");
        const std::string text = scripting::ScriptWriter::write(events);
        EXPECT_NE(text.find("survivalAbove(85.0)"), std::string::npos) << text; // constants are written with a dot
        EXPECT_NE(text.find("survivalBelow(120.5, 1)"), std::string::npos) << text;
        EXPECT_EQ(scripting::ScriptWriter::write(scripting::parse_script(text)), text);
    }

    TEST(BarrierMonitoring, RejectsANonLiteralOrNonPositiveLevel)
    {
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = survivalAbove(h)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = survivalAbove(0)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = survivalBelow(100, 1.5)\n"));
        EXPECT_ANY_THROW(scripting::parse_script("2025-06-03\n    x = survivalBelow()\n"));
    }

    TEST(BarrierMonitoring, BlackScholesRepricesTheContinuousClosedForm)
    {
        ScriptedProduct<Real> prod(kOneEvent, ctx());
        const double T = prod.timeline().front();
        BlackScholesSimModel<Real> model(S0, r, q, vol);
        const auto res = simulate<Real>(prod, model, settings());
        const double exact = down_and_out_call(85.0, T);
        EXPECT_NEAR(res.npv(), exact, 4.0 * res.std_error()) << "closed form " << exact;
        // The barrier matters: well below the vanilla.
        EXPECT_LT(res.npv(), bs_call(S0, K, T) - 10.0 * res.std_error());
    }

    TEST(BarrierMonitoring, InPlusOutIsTheVanilla)
    {
        const std::string in = "2025-06-03\n    pays (1 - survivalAbove(85)) * max(spot() - 100, 0)\n";
        ScriptedProduct<Real> out_prod(kOneEvent, ctx()), in_prod(in, ctx());
        BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
        const auto o = simulate<Real>(out_prod, m1, settings());
        const auto i = simulate<Real>(in_prod, m2, settings());
        const double T = out_prod.timeline().front();
        // Same seed, same paths: the sum is the vanilla path by path.
        EXPECT_NEAR(o.npv() + i.npv(), bs_call(S0, K, T), 4.0 * std::hypot(o.std_error(), i.std_error()));
    }

    TEST(BarrierMonitoring, IntermediateEventsDoNotChangeThePrice)
    {
        ScriptedProduct<Real> one(kOneEvent, ctx()), monthly(kPreamble + kMonthly, ctx());
        const double exact = down_and_out_call(85.0, one.timeline().front());
        BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
        const auto a = simulate<Real>(one, m1, settings());
        const auto b = simulate<Real>(monthly, m2, settings());
        EXPECT_NEAR(a.npv(), exact, 4.0 * a.std_error());
        EXPECT_NEAR(b.npv(), exact, 4.0 * b.std_error());
    }

    TEST(BarrierMonitoring, FlatLocalVolIsBlackScholes)
    {
        ScriptedProduct<Real> prod(kOneEvent, ctx());
        LocalVolSimModel<Real> model(S0, r, q, kK, kT, surface(false), 1.0 / 52.0);
        const auto res = simulate<Real>(prod, model, settings());
        const double exact = down_and_out_call(85.0, prod.timeline().front());
        EXPECT_NEAR(res.npv(), exact, 4.0 * res.std_error()) << "closed form " << exact;
    }

    TEST(BarrierMonitoring, SkewedLocalVolMatchesTheHandWrittenEngine)
    {
        // The hand-written engine on daily steps with its bridge correction,
        // the script on the model's own daily grid with the survival
        // product: both estimate the continuously monitored price.
        ScriptedProduct<Real> prod(kOneEvent, ctx());
        const double T = prod.timeline().front();
        LocalVolSimModel<Real> model(S0, r, q, kK, kT, surface(true), 1.0 / 252.0);
        const auto script = simulate<Real>(prod, model, settings());

        BarrierLocalVolInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = T;
        in.rate = r;
        in.dividend = q;
        in.is_call = true;
        in.barrier_type = BarrierType::DownAndOut;
        in.barrier_level = 85.0;
        in.n_steps = 252;
        in.brownian_bridge = true;
        in.compute_greeks = false;
        in.surface.K_grid = kK;
        in.surface.T_grid = kT;
        in.surface.sigma_loc_flat = surface(true);
        in.n_paths = 200000;
        in.seed = 7;
        const PricingResult engine = default_registry().price(
            {InstrumentKind::EquityBarrierOption, ModelKind::DupireLocalVol, EngineKind::MonteCarlo, PricingInput{in}});

        const double se = std::hypot(script.std_error(), engine.mc_std_error);
        EXPECT_NEAR(script.npv(), engine.npv, 4.0 * se) << "script " << script.npv() << " engine " << engine.npv;
    }

    TEST(BarrierMonitoring, TreeBytecodeAndBatchAgreeToTheBit)
    {
        const std::string src = kPreamble + kMonthly;
        ScriptSettings tree;
        tree.bytecode = false;
        ScriptedProduct<Real> by_tree(src, ctx(), tree), by_code(src, ctx());
        ASSERT_TRUE(by_code.program().has_survival);
        ASSERT_TRUE(by_code.program().batchable());

        LocalVolSimModel<Real> model(S0, r, q, kK, kT, surface(true), 1.0 / 52.0);
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

    TEST(BarrierMonitoring, ModelsWithoutMonitoringRefuseTheScript)
    {
        ScriptedProduct<Real> prod(kOneEvent, ctx());
        MertonJumpDiffusionSimModel<Real> merton(S0, r, q, vol, 0.3, -0.05, 0.1);
        EXPECT_FALSE(merton.monitors_barriers());
        EXPECT_THROW(merton.init(prod.timeline(), prod.defline()), InvalidInput);

        BlackScholesSimModel<Real> bs(S0, r, q, vol);
        EXPECT_TRUE(bs.monitors_barriers());
        EXPECT_NO_THROW(bs.init(prod.timeline(), prod.defline()));
    }

    TEST(BarrierMonitoring, AssetOutOfRangeIsRefused)
    {
        ScriptedProduct<Real> prod("2025-06-03\n    pays survivalAbove(85, 1) * max(spot() - 100, 0)\n", ctx());
        BlackScholesSimModel<Real> bs(S0, r, q, vol);
        EXPECT_THROW(bs.init(prod.timeline(), prod.defline()), InvalidInput);
    }

    TEST(BarrierMonitoring, TheGpuLeavesItToTheCpuAndSaysWhy)
    {
        ScriptedProduct<Real> prod(kOneEvent, ctx());
        DeviceModel dm;
        dm.kind = DeviceModel::Kind::BlackScholes;
        const std::string why = gpu::script_gpu_unsupported(prod.program(), dm, {{}});
        EXPECT_NE(why.find("survivalAbove"), std::string::npos) << why;
    }

} // namespace quantModeling
