#include <gtest/gtest.h>

#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/script_error.hpp"
#include "quantModeling/scripting/visitors/debugger.hpp"
#include "quantModeling/scripting/visitors/script_writer.hpp"

#include <string>
#include <vector>

namespace quantModeling::scripting
{
    namespace
    {
        std::string dump_events(const std::vector<Event> &events)
        {
            std::string out;
            for (const Event &e : events)
            {
                out += e.date.to_iso();
                out += '\n';
                for (const ExprTree &stmt : e.statements)
                    out += Debugger::dump(*stmt);
            }
            return out;
        }

        /// Dump the right-hand side expression of `x = <expr>`.
        std::string dump_expr(const std::string &expression)
        {
            const std::vector<Event> events =
                parse_script("2020-01-01\n    x = " + expression + "\n");
            const Node &assign = *events.at(0).statements.at(0)->arguments.at(0);
            return Debugger::dump(*assign.arguments.at(1));
        }

        /// Dump the condition tree of `if <cond> then ...`.
        std::string dump_cond(const std::string &condition)
        {
            const std::vector<Event> events = parse_script(
                "2020-01-01\n    if " + condition + " then\n        x = 1\n    endIf\n");
            const Node &iff = *events.at(0).statements.at(0)->arguments.at(0);
            return Debugger::dump(*iff.arguments.at(0));
        }

        // ── the round-trip corpus ────────────────────────────────────────────
        std::vector<std::string> corpus()
        {
            return {
                // 1. European call
                "2025-12-16\n    pays max(spot() - 100, 0)\n",
                // 2. European put, strike in a variable
                "2025-06-16\n    k = 95.5\n2025-12-16\n    pays max(k - spot(), 0)\n",
                // 3. digital
                "2025-12-16\n    if spot() > 100 then pays 10 endIf\n",
                // 4. discrete arithmetic Asian
                "2025-03-16\n    acc = spot()\n2025-06-16\n    acc = acc + spot()\n"
                "2025-09-16\n    acc = acc + spot()\n2025-12-16\n"
                "    pays max(acc / 3 - 100, 0)\n",
                // 5. geometric average leg
                "2025-06-16\n    s = log(spot())\n2025-12-16\n"
                "    pays exp((s + log(spot())) / 2)\n",
                // 6. up-and-out call, discrete monitoring
                "2025-06-16\n    alive = 1\n2025-09-15  2025-12-15\n"
                "    if spot() >= 130 then alive = 0 endIf\n"
                "2026-03-16\n    if alive = 1 then pays max(spot() - 100, 0) endIf\n",
                // 7. cliquet with local floor / cap
                "2025-06-16\n    ref = spot()\n    total = 0\n"
                "2025-12-16\n    r = spot() / ref - 1\n"
                "    total = total + min(max(r, 0), 0.05)\n    ref = spot()\n"
                "2026-06-16\n    pays 1000 * total\n",
                // 8. autocall with coupon memory and terminal knock-in (WP §1.1)
                "2025-06-16\n    spot0 = spot()\n    ki = 0\n    alive = 1\n"
                "    period = 1\n\n"
                "2025-09-15  2025-12-15  2026-03-16\n"
                "    if spot() < 0.70 * spot0 then ki = 1 endIf\n"
                "    if alive = 1 and spot() >= spot0 then\n"
                "        pays 1000 * (1 + 0.02 * period)\n"
                "        alive = 0\n"
                "    else\n"
                "        period = period + 1\n"
                "    endIf\n\n"
                "2026-06-15\n    if alive = 1 then\n"
                "        if ki = 1 and spot() < spot0 then\n"
                "            pays 1000 * spot() / spot0\n"
                "        else\n"
                "            pays 1000\n"
                "        endIf\n"
                "    endIf\n",
                // 9. worst-of-two proxy on one name (structure only)
                "2025-12-16\n    a = spot() / 100\n    b = spot() / 110\n"
                "    pays 1000 * min(a, b)\n",
                // 10. power option
                "2025-12-16\n    pays max(spot() ^ 2 - 10000, 0) / 100\n",
                // 11. right-associative power chain
                "2025-12-16\n    pays 2 ^ 3 ^ 2\n",
                // 12. unary minus and plus
                "2025-12-16\n    x = -spot()\n    y = +100\n    pays abs(x + y)\n",
                // 13. nested parentheses
                "2025-12-16\n    pays ((spot() - 100) * (spot() + 100)) / (2 * 100)\n",
                // 14. smoothing requested explicitly
                "2025-12-16\n    if spot() > 100 then pays smooth(spot() - 100, 1) endIf\n",
                // 15. boolean connectives
                "2025-12-16\n"
                "    if spot() > 90 and spot() < 110 or spot() = 100 then pays 1 endIf\n",
                // 16. not with a parenthesised condition
                "2025-12-16\n    if not (spot() = 100) then pays 1 endIf\n",
                // 17. if / else / endIf, multi-statement branches
                "2025-12-16\n    if spot() >= 100 then\n        gain = spot() - 100\n"
                "        pays gain\n    else\n        loss = 100 - spot()\n"
                "        pays 0\n    endIf\n",
                // 18. sqrt and division
                "2025-12-16\n    pays sqrt(spot()) / sqrt(100)\n",
                // 19. case-insensitive keywords
                "2025-12-16\n    IF spot() > 100 THEN\n        PAYS 1\n    ELSE\n"
                "        PAYS 0\n    ENDIF\n",
                // 20. several events, coupon accrual
                "2025-03-16\n    c = 0\n2025-06-16\n    c = c + 25\n"
                "2025-09-16\n    c = c + 25\n2025-12-16\n    c = c + 25\n"
                "    pays 1000 + c\n",
                // 21. chained subtraction (left associative)
                "2025-12-16\n    pays 1000 - spot() - 100\n",
            };
        }
    } // namespace

    // ── round-trip: parse ∘ write is a fixed point of the AST ────────────────

    TEST(ScriptParser, RoundTripCorpus)
    {
        for (const std::string &source : corpus())
        {
            const std::vector<Event> first = parse_script(source);
            const std::string rewritten = ScriptWriter::write(first);
            const std::vector<Event> second = parse_script(rewritten);
            EXPECT_EQ(dump_events(first), dump_events(second))
                << "source:\n"
                << source << "\nrewritten:\n"
                << rewritten;
        }
    }

    TEST(ScriptParser, RewrittenScriptIsStable)
    {
        for (const std::string &source : corpus())
        {
            const std::string once = ScriptWriter::write(parse_script(source));
            const std::string twice = ScriptWriter::write(parse_script(once));
            EXPECT_EQ(once, twice) << "source:\n" << source;
        }
    }

    // ── precedence and associativity are in the tree shape ───────────────────

    TEST(ScriptParser, MultiplicationBindsTighterThanAddition)
    {
        EXPECT_EQ(dump_expr("a + b * c"), dump_expr("a + (b * c)"));
        EXPECT_EQ(dump_expr("a + b * c"),
                  "Add\n  Var a\n  Mult\n    Var b\n    Var c\n");
        EXPECT_EQ(dump_expr("a * b + c"),
                  "Add\n  Mult\n    Var a\n    Var b\n  Var c\n");
    }

    TEST(ScriptParser, PowerIsRightAssociative)
    {
        EXPECT_EQ(dump_expr("a ^ b ^ c"), dump_expr("a ^ (b ^ c)"));
        EXPECT_NE(dump_expr("a ^ b ^ c"), dump_expr("(a ^ b) ^ c"));
    }

    TEST(ScriptParser, SubtractionIsLeftAssociative)
    {
        EXPECT_EQ(dump_expr("a - b - c"), dump_expr("(a - b) - c"));
        EXPECT_NE(dump_expr("a - b - c"), dump_expr("a - (b - c)"));
    }

    TEST(ScriptParser, UnarySignBindsTighterThanPowerPerGrammar)
    {
        // grammar §1.2: factor = unary [ "^" factor ] ; unary = [ +|- ] atom
        EXPECT_EQ(dump_expr("-a ^ b"),
                  "Pow\n  Uminus\n    Var a\n  Var b\n");
    }

    TEST(ScriptParser, AndBindsTighterThanOr)
    {
        EXPECT_EQ(dump_cond("a = 1 and b = 2 or c = 3"),
                  dump_cond("(a = 1 and b = 2) or c = 3"));
        EXPECT_NE(dump_cond("a = 1 and b = 2 or c = 3"),
                  dump_cond("a = 1 and (b = 2 or c = 3)"));
    }

    TEST(ScriptParser, NotAppliesToTheNextConditionElement)
    {
        EXPECT_EQ(dump_cond("not (a = 1)"),
                  "Not\n  Equal\n    Var a\n    Const 1.0\n");
    }

    TEST(ScriptParser, ParenthesesRegroupConditions)
    {
        EXPECT_EQ(dump_cond("(a = 1 or b = 2) and c = 3"),
                  dump_cond("(a = 1 or b = 2) and (c = 3)"));
    }

    // ── shared date line ────────────────────────────────────────────────────

    TEST(ScriptParser, SharedDateLineYieldsOneEventPerDate)
    {
        const std::vector<Event> events = parse_script(
            "2025-09-15  2025-12-15  2026-03-16\n    if spot() < 90 then ki = 1 endIf\n");
        ASSERT_EQ(events.size(), 3u);
        EXPECT_EQ(events[0].date.to_iso(), "2025-09-15");
        EXPECT_EQ(events[1].date.to_iso(), "2025-12-15");
        EXPECT_EQ(events[2].date.to_iso(), "2026-03-16");
        EXPECT_EQ(Debugger::dump(*events[0].statements.at(0)),
                  Debugger::dump(*events[2].statements.at(0)));
    }

    // ── layout ──────────────────────────────────────────────────────────────

    TEST(ScriptParser, CaseInsensitiveKeywords)
    {
        const std::string lower =
            "2025-12-16\n    if spot() > 100 then\n        pays 1\n    else\n"
            "        pays 0\n    endIf\n";
        const std::string upper =
            "2025-12-16\n    IF spot() > 100 THEN\n        PAYS 1\n    ELSE\n"
            "        PAYS 0\n    ENDIF\n";
        EXPECT_EQ(dump_events(parse_script(lower)), dump_events(parse_script(upper)));
    }

    TEST(ScriptParser, OverIndentedContinuationIsAccepted)
    {
        EXPECT_NO_THROW(parse_script("2025-12-16\n    if spot() > 100 then\n"
                                     "            pays 1\n    endIf\n"));
    }

    // ── errors carry a source location ──────────────────────────────────────

    TEST(ScriptParser, UnbalancedParenthesisThrowsScriptError)
    {
        EXPECT_THROW(parse_script("2020-01-01\n    x = (a + b\n"), ScriptError);
    }

    TEST(ScriptParser, MissingEndIfThrowsScriptError)
    {
        EXPECT_THROW(
            parse_script("2020-01-01\n    if a = 1 then\n        x = 1\n"),
            ScriptError);
    }

    TEST(ScriptParser, InvalidCalendarDateThrowsAtItsLine)
    {
        try
        {
            parse_script("2025-13-01\n    x = 1\n");
            FAIL() << "expected ScriptError";
        }
        catch (const ScriptError &e)
        {
            EXPECT_EQ(e.line, 1u);
        }
    }

    TEST(ScriptParser, TrailingTokenAfterStatementThrows)
    {
        EXPECT_THROW(parse_script("2020-01-01\n    x = 1 2\n"), ScriptError);
    }

    TEST(ScriptParser, UnexpectedCharacterThrowsAtItsColumn)
    {
        try
        {
            parse_script("2020-01-01\n    x = a @ b\n");
            FAIL() << "expected ScriptError";
        }
        catch (const ScriptError &e)
        {
            EXPECT_EQ(e.line, 2u);
            EXPECT_EQ(e.col, 11u); // the '@'
        }
    }

    TEST(ScriptParser, UnindentedStatementThrowsScriptError)
    {
        EXPECT_THROW(parse_script("2020-01-01\nx = 1\n"), ScriptError);
    }

    TEST(ScriptParser, PartialDedentThrowsScriptError)
    {
        EXPECT_THROW(parse_script("2020-01-01\n    x = 1\n  y = 2\n"), ScriptError);
    }

    TEST(ScriptParser, EmptyScriptThrowsScriptError)
    {
        EXPECT_THROW(parse_script("   \n\n"), ScriptError);
    }

    TEST(ScriptParser, DateInsideAnExpressionIsRejected)
    {
        // a lone date token where the parser wants a statement / expression
        EXPECT_THROW(parse_script("2020-01-01\n    x = 2019-01-01\n"), ScriptError);
    }

    // ── a few concrete shapes ──────────────────────────────────────────────

    TEST(ScriptParser, SpotAndCallsParseToTheExpectedNodes)
    {
        EXPECT_EQ(dump_expr("spot()"), "Spot\n");
        EXPECT_EQ(dump_expr("min(a, b)"),
                  "Min\n  Var a\n  Var b\n");
        EXPECT_EQ(dump_expr("max(spot() - k, 0)"),
                  "Max\n  Sub\n    Spot\n    Var k\n  Const 0.0\n");
    }

    TEST(ScriptParser, FunctionArityIsChecked)
    {
        EXPECT_THROW(parse_script("2020-01-01\n    x = log(a, b)\n"), ScriptError);
        EXPECT_THROW(parse_script("2020-01-01\n    x = min(a)\n"), ScriptError);
        EXPECT_THROW(parse_script("2020-01-01\n    x = smooth(a)\n"), ScriptError);
    }

} // namespace quantModeling::scripting
