#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"

#include <cmath>

// blueprint/wp/17-aad.md §10 / §14 ("ET | mêmes risques, tape plus petite").
// Number's operators are expression templates now (this lot replaces the
// previous, eager implementation "behind the same API" -- ADR-A5): every
// other AAD test file is the oracle this lot must not disturb (same public
// Number interface, same adjoints, to machine precision -- and indeed all of
// them still pass unchanged). What is specific to this file is the one thing
// only expression templates can prove: how many tape nodes a composite
// expression records.

namespace quantModeling::aad
{
    namespace
    {
        std::size_t count_nodes(Tape &tape)
        {
            std::size_t n = 0;
            for (Tape::iterator it = tape.begin(); it != tape.end(); ++it)
                ++n;
            return n;
        }

        struct FreshTape
        {
            FreshTape() { Number::tape->clear(); }
        };
    } // namespace

    // ── the canonical example from §10: y = x1*x2 + exp(x3) records ONE
    // node (three arguments), not the three (*, exp, +) an eager Number
    // would have recorded before this lot ──────────────────────────────────

    TEST(AADExpression, CompositeExpressionRecordsOneNodeNotThree)
    {
        FreshTape _;
        Number x1(2.0), x2(3.0), x3(0.5);
        const std::size_t before = count_nodes(*Number::tape);

        Number y = x1 * x2 + exp(x3);

        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        EXPECT_NEAR(y.value(), 2.0 * 3.0 + std::exp(0.5), 1e-12);
    }

    TEST(AADExpression, CompositeExpressionMatchesHandDerivedGradient)
    {
        FreshTape _;
        Number x1(2.0), x2(3.0), x3(0.5);

        Number y = x1 * x2 + exp(x3);
        y.propagate_to_start();

        // d/dx1 (x1*x2 + exp(x3)) = x2 ; d/dx2 = x1 ; d/dx3 = exp(x3)
        EXPECT_NEAR(x1.adjoint(), 3.0, 1e-12);
        EXPECT_NEAR(x2.adjoint(), 2.0, 1e-12);
        EXPECT_NEAR(x3.adjoint(), std::exp(0.5), 1e-12);
    }

    // ── a deeper composite, still one node: (x1 - x2) - (x1*x3) * x2 --
    // reused variables (x1, x2 each appear twice) exercise push_adjoint's
    // accumulation into the *same* argument slot from two different
    // branches of the expression tree ──────────────────────────────────────

    TEST(AADExpression, ReusedVariableAccumulatesFromBothBranches)
    {
        FreshTape _;
        Number x1(1.3), x2(-0.4), x3(2.1);
        const std::size_t before = count_nodes(*Number::tape);

        Number y = (x1 - x2) - (x1 * x3) * x2;

        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        y.propagate_to_start();

        // d/dx1 = 1 - x3*x2 ; d/dx2 = -1 - x1*x3 ; d/dx3 = -x1*x2
        EXPECT_NEAR(x1.adjoint(), 1.0 - 2.1 * -0.4, 1e-12);
        EXPECT_NEAR(x2.adjoint(), -1.0 - 1.3 * 2.1, 1e-12);
        EXPECT_NEAR(x3.adjoint(), -1.3 * -0.4, 1e-12);
    }

    // ── mixed with a raw double still collapses to num_numbers == 1
    // (§5.2's "unary node, no wasted leaf"), even nested three deep ────────

    TEST(AADExpression, NestedMixedExpressionStillRecordsOneNode)
    {
        FreshTape _;
        Number x(1.7);
        const std::size_t before = count_nodes(*Number::tape);

        Number y = max(x * 2.0 - 1.0, 0.0);

        EXPECT_EQ(count_nodes(*Number::tape), before + 1);
        y.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), 2.0, 1e-12); // x*2-1 > 0 here, so d/dx = 2
    }

    // ── auto is safe here (ADR-A9: by-value capture), unlike the book's
    // reference-capturing design -- documented as a property, not just
    // asserted in a comment ─────────────────────────────────────────────────

    TEST(AADExpression, AutoDeducedExpressionMaterializesCorrectlyOnUse)
    {
        FreshTape _;
        Number x(4.0), y(5.0);

        auto expr = x * y; // NOLINT -- deliberately testing the auto case
        Number z = expr;   // materializes now; expr's copies of x, y are still fine after this

        EXPECT_NEAR(z.value(), 20.0, 1e-12);
        z.propagate_to_start();
        EXPECT_NEAR(x.adjoint(), 5.0, 1e-12);
        EXPECT_NEAR(y.adjoint(), 4.0, 1e-12);
    }

} // namespace quantModeling::aad
