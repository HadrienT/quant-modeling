#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine_parallel_aad.hpp"
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

            EuroCallT(Real k, Real maturity) : K(k), tl_{maturity}, dl_(1) {}
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

} // namespace quantModeling
