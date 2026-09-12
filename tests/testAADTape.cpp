#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"

#include <vector>

namespace quantModeling::aad
{
    namespace
    {
        /// Points Number::tape at `t` for the scope, restoring whatever it
        /// was before on destruction -- every test below needs an isolated
        /// tape it fully controls, but must never leave the thread_local
        /// pointer dangling at a local Tape once the test returns.
        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        /// A stand-in for one Monte-Carlo path's worth of tape-recorded
        /// arithmetic: a few operations mixing the shared "parameter" p with
        /// a per-path double, and reusing p as the book's own examples do.
        Number one_path_step(Number &p, double xi)
        {
            return p * xi + exp(p) * xi;
        }
    } // namespace

    // ── memory: the tape's block count after N paths equals after path 1 --
    // rewind_to_mark() must reuse the same blocks, never allocate new ones
    // (blueprint §2's "propriété vérifiable") ──────────────────────────────

    TEST(AADTape, MemoryDoesNotGrowAfterTheFirstPath)
    {
        Tape tape;
        TapeSwitch guard(tape);

        Number p(0.6); // a "parameter", before the mark
        tape.mark();

        auto run = [&](double xi)
        {
            tape.rewind_to_mark();
            Number y = one_path_step(p, xi);
            y.propagate_to_mark();
        };

        run(0.3);
        const std::size_t blocks_after_one_path = tape.block_count();

        for (int i = 0; i < 200; ++i)
            run(0.01 * static_cast<double>(i) - 1.0);

        EXPECT_EQ(tape.block_count(), blocks_after_one_path);
    }

    // ── check-pointing: risks with a mark (N small backward passes stopping
    // there, one final push to the parameters) equal risks from recording
    // the whole thing as one sum and propagating once -- the linearity
    // blueprint §7.2 relies on ─────────────────────────────────────────────

    TEST(AADTape, CheckpointedRisksEqualRecordingEverything)
    {
        const std::vector<double> xs = {0.3, -0.7, 1.1, 0.05, -1.4};

        double risk_checkpointed;
        {
            Tape tape;
            TapeSwitch guard(tape);
            Number p(0.6);
            tape.mark();
            for (double xi : xs)
            {
                tape.rewind_to_mark();
                Number y = one_path_step(p, xi);
                y.propagate_to_mark();
            }
            Number::propagate_mark_to_start();
            risk_checkpointed = p.adjoint();
        }

        double risk_recording_everything;
        {
            Tape tape;
            TapeSwitch guard(tape);
            Number p(0.6);
            Number total(0.0);
            for (double xi : xs)
                total = total + one_path_step(p, xi);
            total.propagate_to_start();
            risk_recording_everything = p.adjoint();
        }

        EXPECT_NEAR(risk_checkpointed, risk_recording_everything, 1e-9);
    }

    TEST(AADTape, RewindKeepsMemoryButResetsToTheFirstNode)
    {
        Tape tape;
        TapeSwitch guard(tape);

        Number p(1.0);
        Number q(2.0);
        Number y = p + q;
        y.propagate_to_start();
        ASSERT_NE(p.adjoint(), 0.0);

        tape.rewind();
        Number a(3.0); // reuses the block that used to hold p's node
        EXPECT_EQ(a.value(), 3.0);
        EXPECT_EQ(a.adjoint(), 0.0); // a fresh node: adjoint starts at zero
    }

    // ── the memory ceiling: not in the book (Savine assumes a controlled
    // environment), added because a real tape shares a real, finite machine
    // with everything else running on it. A runaway model must fail loudly
    // well before it can pressure the rest of the process ───────────────────

    TEST(AADTape, ExceedingTheMemoryCeilingThrowsRatherThanGrowingForever)
    {
        Tape tape;
        TapeSwitch guard(tape);
        tape.set_max_bytes(1); // rounds up to exactly one block per pool

        // A lambda call, rather than the loop inlined directly: the raw
        // comma in `Number a(1.0), b(2.0);` would otherwise split
        // EXPECT_THROW's argument at the preprocessor level (it only
        // balances parentheses, not braces).
        auto overflow_the_tape = []()
        {
            for (int i = 0; i < 100000; ++i)
            {
                Number a(1.0);
                Number b(2.0);
                Number c = a + b; // a binary node: draws from every pool
                (void)c;
            }
        };
        EXPECT_THROW(overflow_the_tape(), PricingError);
    }

    TEST(AADTape, DefaultCeilingDoesNotTripOnAnOrdinaryComputation)
    {
        // The out-of-the-box ceiling (256 MB per pool) exists to catch a
        // runaway, not to get in the way of a real path -- even the
        // 50-underlying, 100-step basket the blueprint's own benchmark
        // targets needs only tens of MB (§2). A few thousand operations must
        // sail through untouched.
        Tape tape;
        TapeSwitch guard(tape);
        Number acc(0.0);
        for (int i = 0; i < 5000; ++i)
        {
            Number x(static_cast<double>(i));
            acc = acc + x * 2.0;
        }
        SUCCEED();
    }

} // namespace quantModeling::aad
