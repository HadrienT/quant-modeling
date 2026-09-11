#ifndef QM_SCRIPTING_TOKEN_HPP
#define QM_SCRIPTING_TOKEN_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace quantModeling::scripting
{

    /// One lexical unit of a payoff script.
    ///
    /// The lexer turns source text into a flat stream of these; the parser
    /// (recursive descent, one function per grammar level) consumes it. Layout
    /// is explicit: Newline ends a logical line, Indent / Dedent bracket the
    /// statement block of an event. See blueprint/wp/16-scripting.md §1.2 for
    /// the grammar and §3 for the hand-written lexer / parser (ADR-S1).
    enum class TokenKind
    {
        // ── literals ──────────────────────────────────────────────────────
        Number,     ///< integer or decimal, e.g. 1000, 0.70, 1.02
        Identifier, ///< a script variable, e.g. spot0, ki, period
        DateEvent,  ///< an ISO-8601 calendar date, e.g. 2025-06-16

        // ── keywords (case-insensitive) ───────────────────────────────────
        If,
        Then,
        Else,
        EndIf,
        Pays,
        And,
        Or,
        Not,

        // ── built-in functions ────────────────────────────────────────────
        Min,
        Max,
        Log,
        Exp,
        Sqrt,
        Abs,
        Smooth,
        Spot,

        // ── operators and punctuation ─────────────────────────────────────
        Plus,
        Minus,
        Star,
        Slash,
        Caret,        ///< ^ , right-associative
        Assign,       ///< = ; assignment at statement head, equality in a condition
        NotEqual,     ///< !=
        Less,         ///< <
        LessEqual,    ///< <=
        Greater,      ///< >
        GreaterEqual, ///< >=
        LParen,
        RParen,
        Comma,

        // ── layout ────────────────────────────────────────────────────────
        Newline,
        Indent,
        Dedent,
        Eof
    };

    /// Human-readable spelling of a kind, for error messages and the Debugger.
    std::string_view to_string(TokenKind kind);

    struct Token
    {
        TokenKind kind = TokenKind::Eof;
        std::string lexeme;   ///< exact source text ("" for layout tokens)
        std::size_t line = 1; ///< 1-based line of the first character
        std::size_t col = 1;  ///< 1-based column of the first character
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_TOKEN_HPP
