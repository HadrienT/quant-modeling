#ifndef QM_SCRIPTING_PARSER_HPP
#define QM_SCRIPTING_PARSER_HPP

#include "quantModeling/scripting/event.hpp"
#include "quantModeling/scripting/node.hpp"
#include "quantModeling/scripting/token.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Token stream -> AST, recursive descent, hand-written (ADR-S1).
     *
     * One method per grammar level (blueprint/wp/16-scripting.md §1.2):
     * parse_expr -> parse_term -> parse_factor -> parse_unary -> parse_atom for
     * the arithmetic ladder, parse_cond_or -> parse_cond_and -> parse_cond_elem
     * -> parse_comparison for conditions. `^` is right-associative through the
     * recursion in parse_factor.
     *
     * Each parsed statement is wrapped in a NodeCollect. A shared date line
     * yields one Event per date, each with an independent deep copy of the
     * block.
     *
     * Malformed input throws ScriptError with the offending token's
     * line / column and a pointed extract of the source.
     */
    class Parser
    {
      public:
        Parser(std::vector<Token> tokens, std::string_view source);

        std::vector<Event> parse();

      private:
        const Token &peek(std::size_t ahead = 0) const;
        const Token &current() const;
        bool check(TokenKind kind) const;
        bool match(TokenKind kind);
        const Token &expect(TokenKind kind, const char *what);
        const Token &advance();
        bool at_end() const;
        void skip_newlines();
        [[noreturn]] void fail(const std::string &message,
                               const Token &at) const;

        void parse_event(std::vector<Event> &events);
        ExprTree parse_statement();
        ExprTree parse_assignment();
        ExprTree parse_pays();
        ExprTree parse_if();

        ExprTree parse_condition();
        ExprTree parse_cond_or();
        ExprTree parse_cond_and();
        ExprTree parse_cond_elem();
        ExprTree parse_comparison();
        bool lparen_starts_condition() const;

        ExprTree parse_expr();
        ExprTree parse_term();
        ExprTree parse_factor();
        ExprTree parse_unary();
        ExprTree parse_atom();
        ExprTree parse_call(TokenKind function);

        std::vector<Token> toks_;
        std::string source_;
        std::size_t i_ = 0;
    };

    /// Convenience: lex then parse `source` in one call.
    std::vector<Event> parse_script(const std::string &source);

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_PARSER_HPP
