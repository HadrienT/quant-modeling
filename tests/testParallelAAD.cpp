#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine_parallel_aad.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/rng_interface.hpp"
#include "quantModeling/utils/stats.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using aad::Tape;

        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        template <class T>
        struct EuroCallT final : ISimulatableProduct<T>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            EuroCallT(Real k, Real maturity)
                : K(k), tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max;
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };

        double bs_call(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            const double d2 = d1 - sd;
            return S * std::exp(-q * T) * norm_cdf(d1) -
                   K * std::exp(-r * T) * norm_cdf(d2);
        }

        double bs_delta(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            return std::exp(-q * T) * norm_cdf(d1);
        }

        /// Runs one simulate_parallel_aad() call with `n_workers` in the
        /// pool, fresh model/RNG/tape each time (mirrors how a caller would
        /// actually use it -- state must never leak between runs).
        AADSimulResults run(std::size_t n_workers, std::size_t n_paths,
                            uint64_t seed)
        {
            Tape tape;
            TapeSwitch guard(tape);

            EuroCallT<Number> product(100.0, 1.0);
            BlackScholesSimModel<Number> model{Number(100.0), Number(0.03),
                                               Number(0.01), Number(0.2)};
            Pcg32RNG rng(seed, 0);

            ThreadPool pool;
            pool.start(n_workers);
            const AADSimulResults res =
                simulate_parallel_aad(product, model, pool, rng, n_paths);
            pool.stop();
            return res;
        }
    } // namespace

    TEST(ParallelAAD, ZeroWorkersMatchesBlackScholesAnalytically)
    {
        // With 0 pool workers every batch runs on the calling thread --
        // the degenerate "no real parallelism" case -- and the price/delta
        // should still be statistically correct.
        const AADSimulResults res = run(/*n_workers=*/0, 100000, 5);

        const double expected_price = bs_call(100.0, 100.0, 0.03, 0.01, 0.2, 1.0);
        EXPECT_NEAR(res.price, expected_price, 4.0 * res.price_std_error);

        ASSERT_EQ(res.risk_labels.size(), 4u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        const double expected_delta = bs_delta(100.0, 100.0, 0.03, 0.01, 0.2, 1.0);
        EXPECT_NEAR(res.risks[0], expected_delta, 4.0 * res.risk_std_errors[0]);
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

    TEST(ParallelAAD, WorksWithSobolRNGToo)
    {
        // The RNG interface is meant to be interchangeable -- the same
        // engine, a different generator plugged in.
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 1.0);
        BlackScholesSimModel<Number> model{Number(100.0), Number(0.03),
                                           Number(0.01), Number(0.2)};
        SobolRNG rng(3);

        ThreadPool pool;
        pool.start(4);
        const AADSimulResults res =
            simulate_parallel_aad(product, model, pool, rng, 100000);
        pool.stop();

        const double expected_price = bs_call(100.0, 100.0, 0.03, 0.01, 0.2, 1.0);
        EXPECT_NEAR(res.price, expected_price, 4.0 * res.price_std_error);
    }

    // ── the book's own acceptance criterion (§8.4 / §15's 17d row): bit-
    // for-bit identical price AND risks across 1/2/8 worker threads ────────

    TEST(ParallelAAD, PriceAndRisksAreBitForBitAcrossThreadCounts)
    {
        constexpr std::size_t n_paths = 50000;
        constexpr uint64_t seed = 7;

        const AADSimulResults r0 = run(0, n_paths, seed);
        const AADSimulResults r1 = run(1, n_paths, seed);
        const AADSimulResults r2 = run(2, n_paths, seed);
        const AADSimulResults r8 = run(8, n_paths, seed);

        for (const AADSimulResults *r : {&r1, &r2, &r8})
        {
            EXPECT_EQ(r0.price, r->price);
            EXPECT_EQ(r0.price_std_error, r->price_std_error);
            ASSERT_EQ(r0.risks.size(), r->risks.size());
            for (std::size_t j = 0; j < r0.risks.size(); ++j)
            {
                EXPECT_EQ(r0.risks[j], r->risks[j]) << "risk " << j;
                EXPECT_EQ(r0.risk_std_errors[j], r->risk_std_errors[j])
                    << "risk " << j;
            }
        }
    }

    TEST(ParallelAAD, DifferentSeedsGiveDifferentResults)
    {
        // A sanity check that the reproducibility above isn't a trivial
        // artifact of e.g. every path silently drawing the same gaussians:
        // changing the seed must actually change the price.
        const AADSimulResults a = run(2, 20000, 1);
        const AADSimulResults b = run(2, 20000, 2);
        EXPECT_NE(a.price, b.price);
    }

    // ── simulate_aad on a pool (WP 26): scripts, Sobol replicates and Philox
    // batches on their own tapes, the same bits as on one thread ───────────

    namespace
    {
        AADSimulResults script_aad(const scripting::ScriptModelSpec &spec, const std::string &script,
                                   SamplerKind sampler, RngKind rng, std::size_t workers,
                                   std::vector<std::vector<Real>> *batches = nullptr)
        {
            const ValuationContext ctx{Date::from_iso("2026-11-16")};
            const ScriptedProduct<Number> product(script, ctx);
            auto model = scripting::make_script_model<Number>(spec);
            ThreadPool pool;
            if (workers > 0)
                pool.start(workers);
            return simulate_aad(product, *model, 4096, 7, first_aad_payoff, rng, sampler, batches,
                                workers > 0 ? &pool : nullptr);
        }

        void expect_same_bits(const AADSimulResults &a, const AADSimulResults &b)
        {
            EXPECT_EQ(a.price, b.price);
            EXPECT_EQ(a.price_std_error, b.price_std_error);
            ASSERT_EQ(a.risks.size(), b.risks.size());
            for (std::size_t j = 0; j < a.risks.size(); ++j)
            {
                EXPECT_EQ(a.risks[j], b.risks[j]) << a.risk_labels[j];
                EXPECT_EQ(a.risk_std_errors[j], b.risk_std_errors[j]) << a.risk_labels[j];
            }
        }

        scripting::ScriptModelSpec rough_bergomi_spec()
        {
            scripting::ScriptModelSpec s;
            s.model = "rough_bergomi";
            s.spot = 100.0;
            s.rate = 0.03;
            s.dividend = 0.01;
            s.rb_H = 0.1;
            s.rb_eta = 1.9;
            s.rb_rho = -0.9;
            s.xi_times = {0.5, 1.0};
            s.xi_values = {0.04, 0.05};
            s.max_dt = 1.0 / 252.0;
            return s;
        }
    } // namespace

    TEST(SimulateAadOnAPool, AScriptsRisksDoNotDependOnTheThreadCount)
    {
        const std::string barrier = "2027-05-16\n    s1 = spot()\n"
                                    "2027-11-16\n    if s1 > 95 then pays max(spot() - 100, 0) endIf\n";
        scripting::ScriptModelSpec heston;
        heston.model = "heston";
        heston.spot = 100.0;
        heston.rate = 0.03;
        heston.heston = HestonParams{0.04, 1.5, 0.04, 0.5, -0.7};
        for (const scripting::ScriptModelSpec &spec : {rough_bergomi_spec(), heston})
            for (const SamplerKind sampler : {SamplerKind::Sobol, SamplerKind::PseudoRandom})
            {
                std::vector<std::vector<Real>> one_batches, four_batches;
                const AADSimulResults one = script_aad(spec, barrier, sampler, RngKind::Philox, 0, &one_batches);
                const AADSimulResults four = script_aad(spec, barrier, sampler, RngKind::Philox, 3, &four_batches);
                SCOPED_TRACE(spec.model);
                expect_same_bits(one, four);
                EXPECT_EQ(one_batches, four_batches);
                EXPECT_NE(four.diagnostics.find("on 4 threads"), std::string::npos) << four.diagnostics;
                EXPECT_GT(one.risks.front(), 0.0); // delta of a call
            }
    }

    TEST(SimulateAadOnAPool, PCG32StaysOnOneThread)
    {
        const std::string call = "2027-11-16\n    pays max(spot() - 100, 0)\n";
        const AADSimulResults one = script_aad(rough_bergomi_spec(), call, SamplerKind::PseudoRandom, RngKind::Pcg32, 0);
        const AADSimulResults four = script_aad(rough_bergomi_spec(), call, SamplerKind::PseudoRandom, RngKind::Pcg32, 3);
        expect_same_bits(one, four);
        EXPECT_EQ(four.diagnostics.find("threads"), std::string::npos) << four.diagnostics;
    }

    TEST(SimulateAadOnAPool, AScriptWithEarlyExerciseRunsOnEveryThreadWithTheSameBits)
    {
        // The tree evaluator's per-thread copy (ScriptedProduct::TreeState):
        // a Bermudan put under the fitted rule, its price and its tape's
        // risks, on one thread and on four.
        const ValuationContext ctx{Date::from_iso("2026-11-16")};
        std::string script;
        for (int m = 1; m <= 6; ++m)
            script += "2027-0" + std::to_string(m) +
                      "-16\n    if exercise(max(100 - spot(), 0), spot()) then\n        pays max(100 - spot(), 0)\n"
                      "    endIf\n";
        scripting::ScriptModelSpec spec;
        spec.model = "black_scholes";
        spec.spot = 100.0;
        spec.rate = 0.03;
        spec.vol = 0.25;
        ScriptedProduct<Real> priced(script, ctx);
        auto model = scripting::make_script_model<Real>(spec);
        fit_exercise_policy(priced, *model, 4096, 3);
        ASSERT_TRUE(priced.has_exercise());
        ASSERT_NE(priced.make_payoff_state(), nullptr);

        const auto aad = [&](std::size_t workers)
        {
            ScriptedProduct<Number> product(script, ctx);
            product.set_exercise_policy(priced.exercise_policy());
            auto m = scripting::make_script_model<Number>(spec);
            ThreadPool pool;
            if (workers > 0)
                pool.start(workers);
            return simulate_aad(product, *m, 4096, 7, first_aad_payoff, RngKind::Philox, SamplerKind::Sobol, nullptr,
                                workers > 0 ? &pool : nullptr);
        };
        const AADSimulResults one = aad(0), four = aad(3);
        expect_same_bits(one, four);
        EXPECT_NE(four.diagnostics.find("on 4 threads"), std::string::npos) << four.diagnostics;
        EXPECT_GT(one.price, 0.0);
        EXPECT_LT(one.risks.front(), 0.0); // a put's delta

        // The price engine too.
        const auto price = [&](std::size_t workers)
        {
            auto m = scripting::make_script_model<Real>(spec);
            PricingSettings s;
            s.mc_paths = 8192;
            s.mc_seed = 5;
            s.mc_sampler = SamplerKind::Sobol;
            ThreadPool pool;
            if (workers > 0)
                pool.start(workers);
            return simulate_script(priced, *m, s, workers > 0 ? &pool : nullptr);
        };
        const SimulationMCResult p1 = price(0), p4 = price(3);
        EXPECT_EQ(p1.npv(), p4.npv());
        EXPECT_EQ(p1.std_error(), p4.std_error());
    }

} // namespace quantModeling
