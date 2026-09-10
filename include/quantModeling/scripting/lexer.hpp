#ifndef QM_SCRIPTING_LEXER_HPP
#define QM_SCRIPTING_LEXER_HPP

#include "quantModeling/scripting/token.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief Source text -> flat token stream, hand-written (ADR-S1).
     *
     * Layout handling (blueprint/wp/16-scripting.md §1.2 grammar):
     *  - a line that is blank or all-whitespace produces no token;
     *  - a `\n` ends a logical line and yields a Newline token;
     *  - the statement block under an event date is bracketed by one Indent /
     *    Dedent pair. Only that single level is tracked: further indentation
     *    inside an `if ... then ... endIf` body is not significant, the keywords
     *    delimit the branches.
     *
     * Keywords and function names are case-insensitive. An ISO-8601 date
     * `YYYY-MM-DD` is lexed as one DateEvent token before `-` is considered a
     * minus, so `2025-06-16` is never `2025 - 6 - 16`. A `#` starts a comment
     * that runs to the end of the line.
     *
     * On malformed input the lexer throws ScriptError with the offending
     * line / column and a pointed extract.
     */
    class Lexer
    {
      public:
        explicit Lexer(std::string source);

        /// Tokenize the whole source. The stream always ends with Eof (and a
        /// closing Dedent if a block was still open).
        std::vector<Token> tokenize();

      private:
        char peek(std::size_t ahead = 0) const;
        bool at_end() const;
        char advance();
        void add(TokenKind kind, std::string lexeme, std::size_t line,
                 std::size_t col);
        [[noreturn]] void fail(const std::string &message, std::size_t line,
                               std::size_t col) const;

        void lex_line_body();
        void lex_number_or_date();
        void lex_identifier_or_keyword();
        std::size_t measure_indent(); ///< leading-whitespace width; advances past it

        std::string src_;
        std::size_t pos_ = 0;
        std::size_t line_ = 1;
        std::size_t col_ = 1;

        std::vector<Token> out_;
        bool in_block_ = false;      ///< an event body is open (Indent emitted)
        std::size_t block_indent_ = 0;
        std::size_t header_indent_ = 0;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_LEXER_HPP
