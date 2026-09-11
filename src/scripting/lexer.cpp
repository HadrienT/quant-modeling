#include "quantModeling/scripting/lexer.hpp"

#include "quantModeling/scripting/script_error.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace quantModeling::scripting
{

    namespace
    {
        bool is_digit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool is_ident_start(char c)
        {
            return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        bool is_ident_char(char c)
        {
            return is_ident_start(c) || is_digit(c);
        }

        char lower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /// Keyword / builtin lookup on an already-lowercased spelling.
        TokenKind keyword_kind(const std::string &lowered, bool &is_keyword)
        {
            static const std::array<std::pair<const char *, TokenKind>, 16> table{
                {{"if", TokenKind::If},
                 {"then", TokenKind::Then},
                 {"else", TokenKind::Else},
                 {"endif", TokenKind::EndIf},
                 {"pays", TokenKind::Pays},
                 {"and", TokenKind::And},
                 {"or", TokenKind::Or},
                 {"not", TokenKind::Not},
                 {"min", TokenKind::Min},
                 {"max", TokenKind::Max},
                 {"log", TokenKind::Log},
                 {"exp", TokenKind::Exp},
                 {"sqrt", TokenKind::Sqrt},
                 {"abs", TokenKind::Abs},
                 {"smooth", TokenKind::Smooth},
                 {"spot", TokenKind::Spot}}};
            for (const auto &[text, kind] : table)
                if (lowered == text)
                {
                    is_keyword = true;
                    return kind;
                }
            is_keyword = false;
            return TokenKind::Identifier;
        }
    } // namespace

    Lexer::Lexer(std::string source)
        : src_(std::move(source)) {}

    bool Lexer::at_end() const
    {
        return pos_ >= src_.size();
    }

    char Lexer::peek(std::size_t ahead) const
    {
        const std::size_t i = pos_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }

    char Lexer::advance()
    {
        const char c = src_[pos_++];
        if (c == '\n')
        {
            ++line_;
            col_ = 1;
        }
        else
        {
            ++col_;
        }
        return c;
    }

    void Lexer::add(TokenKind kind, std::string lexeme, std::size_t line,
                    std::size_t col)
    {
        out_.push_back(Token{kind, std::move(lexeme), line, col});
    }

    void Lexer::fail(const std::string &message, std::size_t line,
                     std::size_t col) const
    {
        throw ScriptError(message, line, col, source_line_at(src_, line));
    }

    std::size_t Lexer::measure_indent()
    {
        std::size_t width = 0;
        while (!at_end() && (peek() == ' ' || peek() == '\t'))
        {
            advance();
            ++width;
        }
        return width;
    }

    void Lexer::lex_number_or_date()
    {
        const std::size_t sl = line_;
        const std::size_t sc = col_;
        std::string s;
        while (is_digit(peek()))
            s += advance();

        const bool looks_like_date =
            s.size() == 4 && peek() == '-' && is_digit(peek(1)) &&
            is_digit(peek(2)) && peek(3) == '-' && is_digit(peek(4)) &&
            is_digit(peek(5));
        if (looks_like_date)
        {
            for (int i = 0; i < 6; ++i) // -MM-DD, 6 more characters
                s += advance();
            if (is_digit(peek()) || is_ident_start(peek()) || peek() == '-')
                fail("malformed date literal '" + s + "'", sl, sc);
            add(TokenKind::DateEvent, std::move(s), sl, sc);
            return;
        }

        if (peek() == '.' && is_digit(peek(1)))
        {
            s += advance(); // '.'
            while (is_digit(peek()))
                s += advance();
        }
        if (peek() == '.' || is_ident_start(peek()))
            fail("malformed number '" + s + std::string(1, peek()) + "'", sl, sc);
        add(TokenKind::Number, std::move(s), sl, sc);
    }

    void Lexer::lex_identifier_or_keyword()
    {
        const std::size_t sl = line_;
        const std::size_t sc = col_;
        std::string s;
        while (is_ident_char(peek()))
            s += advance();

        std::string lowered;
        lowered.reserve(s.size());
        for (char c : s)
            lowered += lower(c);

        bool is_keyword = false;
        const TokenKind kind = keyword_kind(lowered, is_keyword);
        add(is_keyword ? kind : TokenKind::Identifier, std::move(s), sl, sc);
    }

    void Lexer::lex_line_body()
    {
        while (!at_end() && peek() != '\n' && peek() != '\r')
        {
            const char c = peek();
            if (c == ' ' || c == '\t')
            {
                advance();
                continue;
            }
            if (c == '#') // comment to end of line
            {
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
                break;
            }

            const std::size_t sl = line_;
            const std::size_t sc = col_;

            if (is_digit(c))
            {
                lex_number_or_date();
                continue;
            }
            if (is_ident_start(c))
            {
                lex_identifier_or_keyword();
                continue;
            }

            switch (c)
            {
                case '+':
                    advance();
                    add(TokenKind::Plus, "+", sl, sc);
                    break;
                case '-':
                    advance();
                    add(TokenKind::Minus, "-", sl, sc);
                    break;
                case '*':
                    advance();
                    add(TokenKind::Star, "*", sl, sc);
                    break;
                case '/':
                    advance();
                    add(TokenKind::Slash, "/", sl, sc);
                    break;
                case '^':
                    advance();
                    add(TokenKind::Caret, "^", sl, sc);
                    break;
                case '(':
                    advance();
                    add(TokenKind::LParen, "(", sl, sc);
                    break;
                case ')':
                    advance();
                    add(TokenKind::RParen, ")", sl, sc);
                    break;
                case ',':
                    advance();
                    add(TokenKind::Comma, ",", sl, sc);
                    break;
                case '=':
                    advance();
                    add(TokenKind::Assign, "=", sl, sc);
                    break;
                case '!':
                    advance();
                    if (peek() != '=')
                        fail("expected '=' after '!'", sl, sc);
                    advance();
                    add(TokenKind::NotEqual, "!=", sl, sc);
                    break;
                case '<':
                    advance();
                    if (peek() == '=')
                    {
                        advance();
                        add(TokenKind::LessEqual, "<=", sl, sc);
                    }
                    else
                    {
                        add(TokenKind::Less, "<", sl, sc);
                    }
                    break;
                case '>':
                    advance();
                    if (peek() == '=')
                    {
                        advance();
                        add(TokenKind::GreaterEqual, ">=", sl, sc);
                    }
                    else
                    {
                        add(TokenKind::Greater, ">", sl, sc);
                    }
                    break;
                default:
                    fail(std::string("unexpected character '") + c + "'", sl, sc);
            }
        }
    }

    std::vector<Token> Lexer::tokenize()
    {
        bool expect_body = false;

        while (!at_end())
        {
            const std::size_t indent = measure_indent();

            // Blank, whitespace-only, or comment-only line: no tokens, no
            // layout change.
            if (at_end() || peek() == '\n' || peek() == '\r' || peek() == '#')
            {
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance(); // skip a trailing comment
                if (peek() == '\r')
                    advance();
                if (peek() == '\n')
                    advance();
                continue;
            }

            const std::size_t body_line = line_;

            if (expect_body)
            {
                if (indent <= header_indent_)
                    fail("the statements of an event must be indented under its "
                         "date",
                         body_line, indent + 1);
                add(TokenKind::Indent, "", body_line, 1);
                in_block_ = true;
                block_indent_ = indent;
                expect_body = false;
            }
            else if (in_block_)
            {
                if (indent < block_indent_)
                {
                    if (indent != header_indent_)
                        fail("unexpected indentation; expected a new event date "
                             "at column 1",
                             body_line, indent + 1);
                    add(TokenKind::Dedent, "", body_line, 1);
                    in_block_ = false;
                }
            }

            const bool is_header = !in_block_ && !expect_body;
            if (is_header && indent != header_indent_)
                fail("an event date must start at column 1", body_line,
                     indent + 1);

            lex_line_body();

            // Close the logical line.
            add(TokenKind::Newline, "", line_, col_);
            if (peek() == '\r')
                advance();
            if (peek() == '\n')
                advance();

            if (is_header)
                expect_body = true;
        }

        if (expect_body)
            fail("event date has no statements", line_, 1);
        if (in_block_)
            add(TokenKind::Dedent, "", line_, col_);
        add(TokenKind::Eof, "", line_, col_);
        return out_;
    }

} // namespace quantModeling::scripting
