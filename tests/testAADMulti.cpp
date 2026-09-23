#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

// blueprint/wp/17-aad.md §9 / §14 ("Multi | matrice multi-adjoints = m AAD
// mono-adjoint"): one backward pass seeding m independent adjoint components
// must give the same m gradients as m separate mono-adjoint passes.

namespace quantModeling::aad
{
    namespace
    {
        /// Points Number::tape at `t` for the scope, restoring whatever it
        /// was before on destruction -- same pattern as testAADTape.cpp and
        /// testAADSimulation.cpp.
        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        /// Turns Tape::multi / Node::num_adj on for the scope, restoring
        /// both on exit -- both are plain statics (§17f is single-threaded
        /// by design, see simulation_engine_aad.hpp's detail::MultiModeGuard),
        /// so a test that forgets to turn multi back off would corrupt every
        /// mono-adjoint test that happens to run afterward in the same
        /// binary.
        struct MultiSwitch
        {
            std::size_t saved_num_adj = Node::num_adj;
            bool saved_multi = Tape::is_multi();
            explicit MultiSwitch(std::size_t m)
            {
                Node::num_adj = m;
                Tape::set_multi(true);
            }
            ~MultiSwitch()
            {
                Node::num_adj = saved_num_adj;
                Tape::set_multi(saved_multi);
            }
        };
    } // namespace

    // ── the primitive: two independent outputs of a shared parameter,
    // differentiated in one multi-adjoint pass vs. two separate mono-adjoint
    // passes ─────────────────────────────────────────────────────────────

    TEST(AADMulti, MultiAdjointMatrixMatchesTwoMonoRuns)
    {
        const double p0 = 0.6, xi = 0.3;
        auto y0_of = [xi](Number &p)
        { return p * xi + exp(p) * xi; };
        auto y1_of = [](Number &p)
        { return p * p + log(p); };

        double d_y0_dp_multi, d_y1_dp_multi;
        {
            Tape tape;
            TapeSwitch tape_guard(tape);
            MultiSwitch multi_guard(2);

            Number p(p0);
            Number y0 = y0_of(p);
            Number y1 = y1_of(p);
            y0.adjoint(0) = 1.0;
            y1.adjoint(1) = 1.0;
            Number::propagate_to_start_multi();
            d_y0_dp_multi = p.adjoint(0);
            d_y1_dp_multi = p.adjoint(1);
        }

        double d_y0_dp_mono, d_y1_dp_mono;
        {
            Tape tape;
            TapeSwitch tape_guard(tape);
            Number p(p0);
            Number y0 = y0_of(p);
            y0.propagate_to_start();
            d_y0_dp_mono = p.adjoint();
        }
        {
            Tape tape;
            TapeSwitch tape_guard(tape);
            Number p(p0);
            Number y1 = y1_of(p);
            y1.propagate_to_start();
            d_y1_dp_mono = p.adjoint();
        }

        EXPECT_NEAR(d_y0_dp_multi, d_y0_dp_mono, 1e-12);
        EXPECT_NEAR(d_y1_dp_multi, d_y1_dp_mono, 1e-12);
    }

    // ── check-pointing extends to multi mode: risks accumulated over many
    // small propagate_to_mark_multi() calls, reset between batches, equal
    // recording everything and propagating once -- the same property
    // testAADTape.cpp's CheckpointedRisksEqualRecordingEverything proves for
    // one adjoint, now for two at once. This is what actually exercises
    // Node::reset()'s multi-row branch, which nothing else does. ──────────

    TEST(AADMulti, CheckpointedMultiRisksEqualRecordingEverything)
    {
        const std::vector<double> xs = {0.3, -0.7, 1.1, 0.05, -1.4};

        double risk0_checkpointed, risk1_checkpointed;
        {
            Tape tape;
            TapeSwitch tape_guard(tape);
            MultiSwitch multi_guard(2);

            Number p(0.6);
            tape.mark();
            for (double xi : xs)
            {
                tape.rewind_to_mark();
                Number y0 = p * xi + exp(p) * xi;
                Number y1 = p * p * xi;
                y0.adjoint(0) = 1.0;
                y1.adjoint(1) = 1.0;
                Number::propagate_to_mark_multi();
            }
            Number::propagate_mark_to_start_multi();
            risk0_checkpointed = p.adjoint(0);
            risk1_checkpointed = p.adjoint(1);
        }

        double risk0_full, risk1_full;
        {
            Tape tape;
            TapeSwitch tape_guard(tape);
            MultiSwitch multi_guard(2);

            Number p(0.6);
            Number total0(0.0), total1(0.0);
            for (double xi : xs)
            {
                total0 = total0 + (p * xi + exp(p) * xi);
                total1 = total1 + (p * p * xi);
            }
            total0.adjoint(0) = 1.0;
            total1.adjoint(1) = 1.0;
            Number::propagate_to_start_multi();
            risk0_full = p.adjoint(0);
            risk1_full = p.adjoint(1);
        }

        EXPECT_NEAR(risk0_checkpointed, risk0_full, 1e-9);
        EXPECT_NEAR(risk1_checkpointed, risk1_full, 1e-9);
    }

} // namespace quantModeling::aad

// ── the integration test: simulate_aad_multi on a two-payoff product against
// two separate simulate_aad runs, same seed -- both walk the exact same
// random path draws, so the two must agree, not just statistically ────────

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

        /// A European call and put on the same underlying, same maturity --
        /// two genuinely different payoffs sharing everything upstream of
        /// them (the simulated spot), which is exactly what a portfolio's
        /// simulate_aad_multi call needs to get right.
        template <class T>
        struct EuroCallPutT final : ISimulatableProduct<T>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"call", "put"};

            EuroCallPutT(Real k, Real maturity)
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
                using std::max; // ADL: aad::max for T = Number
                const T &spot = p[0].spots[0];
                out.resize(2);
                out[0] = max(spot - K, 0.0) / p[0].numeraire;
                out[1] = max(K - spot, 0.0) / p[0].numeraire;
            }
        };
    } // namespace

    TEST(AADMulti, SimulateAadMultiMatchesTwoSeparateMonoRuns)
    {
        const Real S0 = 100, r = 0.03, q = 0.01, sigma = 0.2, K = 105, Tm = 1.0;
        const std::size_t n_paths = 5000;
        const std::uint64_t seed = 11;

        EuroCallPutT<Number> product(K, Tm);

        Tape tape;
        TapeSwitch guard(tape);
        BlackScholesSimModel<Number> model{Number(S0), Number(r), Number(q),
                                           Number(sigma)};
        const AADMultiSimulResults multi = simulate_aad_multi(product, model, n_paths, seed);

        ASSERT_EQ(multi.payoff_labels.size(), 2u);
        EXPECT_EQ(multi.payoff_labels[0], "call");
        EXPECT_EQ(multi.payoff_labels[1], "put");
        ASSERT_EQ(multi.risk_labels.size(), 4u);
        const std::size_t n_params = multi.risk_labels.size();

        Tape tape_call;
        TapeSwitch guard_call(tape_call);
        BlackScholesSimModel<Number> model_call{Number(S0), Number(r), Number(q),
                                                Number(sigma)};
        const AADSimulResults call_mono = simulate_aad(
            product, model_call, n_paths, seed,
            [](const std::vector<Number> &payoffs)
            { return payoffs[0]; });

        Tape tape_put;
        TapeSwitch guard_put(tape_put);
        BlackScholesSimModel<Number> model_put{Number(S0), Number(r), Number(q),
                                               Number(sigma)};
        const AADSimulResults put_mono = simulate_aad(
            product, model_put, n_paths, seed,
            [](const std::vector<Number> &payoffs)
            { return payoffs[1]; });

        EXPECT_NEAR(multi.prices[0], call_mono.price, 1e-9);
        EXPECT_NEAR(multi.prices[1], put_mono.price, 1e-9);
        for (std::size_t j = 0; j < n_params; ++j)
        {
            EXPECT_NEAR(multi.risks[0 * n_params + j], call_mono.risks[j], 1e-9);
            EXPECT_NEAR(multi.risks[1 * n_params + j], put_mono.risks[j], 1e-9);
        }
    }

} // namespace quantModeling
