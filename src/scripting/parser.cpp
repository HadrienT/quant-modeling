#include "quantModeling/scripting/parser.hpp"

#include "quantModeling/scripting/lexer.hpp"
#include "quantModeling/scripting/script_error.hpp"

#include "quantModeling/core/date.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace quantModeling::scripting
{

    namespace
    {
        template <class N>
        ExprTree make_unary_node(ExprTree child)
        {
            auto node = std::make_unique<N>();
            node->arguments.push_back(std::move(child));
            return node;
        }

        template <class N>
        ExprTree make_binary_node(ExprTree lhs, ExprTree rhs)
        {
            auto node = std::make_unique<N>();
            node->arguments.push_back(std::move(lhs));
            node->arguments.push_back(std::move(rhs));
            return node;
        }

        bool is_condition_operator(TokenKind kind)
        {
            switch (kind)
            {
            case TokenKind::And:
            case TokenKind::Or:
            case TokenKind::Not:
            case TokenKind::Assign:
            case TokenKind::NotEqual:
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                return true;
            default:
                return false;
            }
        }
    } // namespace

    Parser::Parser(std::vector<Token> tokens, std::string_view source)
        : toks_(std::move(tokens)), source_(source)
    {
    }

    // ── cursor ───────────────────────────────────────────────────────────────

    const Token &Parser::current() const { return toks_[i_]; }

    const Token &Parser::peek(std::size_t ahead) const
    {
        const std::size_t j = i_ + ahead;
        return j < toks_.size() ? toks_[j] : toks_.back();
    }

    bool Parser::check(TokenKind kind) const { return current().kind == kind; }

    bool Parser::at_end() const { return current().kind == TokenKind::Eof; }

    const Token &Parser::advance()
    {
        const Token &tok = toks_[i_];
        if (i_ + 1 < toks_.size())
            ++i_;
        return tok;
    }

    bool Parser::match(TokenKind kind)
    {
        if (!check(kind))
            return false;
        advance();
        return true;
    }

    const Token &Parser::expect(TokenKind kind, const char *what)
    {
        if (check(kind))
            return advance();
        fail(std::string("expected ") + what + ", found " +
                 std::string(to_string(current().kind)),
             current());
    }

    void Parser::skip_newlines()
    {
        while (check(TokenKind::Newline))
            advance();
    }

    void Parser::fail(const std::string &message, const Token &at) const
    {
        throw ScriptError(message, at.line, at.col,
                          source_line_at(source_, at.line));
    }

    // ── top level ────────────────────────────────────────────────────────────

    std::vector<Event> Parser::parse()
    {
        std::vector<Event> events;
        skip_newlines();
        while (!at_end())
        {
            parse_event(events);
            skip_newlines();
        }
        if (events.empty())
            fail("empty script: expected at least one dated event", current());
        return events;
    }

    void Parser::parse_event(std::vector<Event> &events)
    {
        auto to_date = [this](const Token &tok)
        {
            try
            {
                return Date::from_iso(tok.lexeme);
            }
            catch (const std::exception &)
            {
                fail("not a valid calendar date: '" + tok.lexeme + "'", tok);
            }
        };

        std::vector<Date> dates;
        dates.push_back(to_date(expect(TokenKind::DateEvent,
                                       "an event date (YYYY-MM-DD)")));
        while (check(TokenKind::DateEvent))
            dates.push_back(to_date(advance()));

        expect(TokenKind::Newline, "a line break after the event date");
        expect(TokenKind::Indent, "an indented statement block");

        std::vector<ExprTree> statements;
        skip_newlines();
        while (!check(TokenKind::Dedent))
        {
            if (at_end())
                fail("unexpected end of script inside an event block", current());
            statements.push_back(parse_statement());
            skip_newlines();
        }
        expect(TokenKind::Dedent, "the end of the event block");

        for (std::size_t k = 0; k < dates.size(); ++k)
        {
            Event event;
            event.date = dates[k];
            if (k + 1 < dates.size())
            {
                event.statements.reserve(statements.size());
                for (const ExprTree &s : statements)
                    event.statements.push_back(s->clone());
            }
            else
            {
                event.statements = std::move(statements);
            }
            events.push_back(std::move(event));
        }
    }

    // ── statements ───────────────────────────────────────────────────────────

    ExprTree Parser::parse_statement()
    {
        ExprTree inner;
        if (check(TokenKind::If))
            inner = parse_if();
        else if (check(TokenKind::Pays))
            inner = parse_pays();
        else if (check(TokenKind::Identifier))
            inner = parse_assignment();
        else
            fail("expected a statement: an assignment, 'pays', or 'if'",
                 current());

        if (!check(TokenKind::Newline) && !check(TokenKind::Else) &&
            !check(TokenKind::EndIf) && !check(TokenKind::Dedent) && !at_end())
            fail("unexpected token after the statement", current());
        skip_newlines();

        return make_unary_node<NodeCollect>(std::move(inner));
    }

    ExprTree Parser::parse_assignment()
    {
        const Token &id = expect(TokenKind::Identifier, "a variable name");
        expect(TokenKind::Assign, "'=' in the assignment");

        auto var = std::make_unique<NodeVar>();
        var->name = id.lexeme;

        auto node = std::make_unique<NodeAssign>();
        node->arguments.push_back(std::move(var));
        node->arguments.push_back(parse_expr());
        return node;
    }

    ExprTree Parser::parse_pays()
    {
        expect(TokenKind::Pays, "'pays'");
        return make_unary_node<NodePays>(parse_expr());
    }

    ExprTree Parser::parse_if()
    {
        expect(TokenKind::If, "'if'");

        auto node = std::make_unique<NodeIf>();
        node->arguments.push_back(parse_condition());
        expect(TokenKind::Then, "'then' after the condition");
        skip_newlines();

        while (!check(TokenKind::Else) && !check(TokenKind::EndIf))
        {
            if (at_end() || check(TokenKind::Dedent))
                fail("unterminated 'if': expected 'else' or 'endIf'", current());
            node->arguments.push_back(parse_statement());
        }
        node->firstElse = node->arguments.size();

        if (match(TokenKind::Else))
        {
            skip_newlines();
            while (!check(TokenKind::EndIf))
            {
                if (at_end() || check(TokenKind::Dedent))
                    fail("unterminated 'if': expected 'endIf'", current());
                node->arguments.push_back(parse_statement());
            }
        }
        expect(TokenKind::EndIf, "'endIf' to close the 'if'");
        return node;
    }

    // ── conditions ───────────────────────────────────────────────────────────

    ExprTree Parser::parse_condition() { return parse_cond_or(); }

    ExprTree Parser::parse_cond_or()
    {
        ExprTree left = parse_cond_and();
        while (match(TokenKind::Or))
            left = make_binary_node<NodeOr>(std::move(left), parse_cond_and());
        return left;
    }

    ExprTree Parser::parse_cond_and()
    {
        ExprTree left = parse_cond_elem();
        while (match(TokenKind::And))
            left = make_binary_node<NodeAnd>(std::move(left), parse_cond_elem());
        return left;
    }

    bool Parser::lparen_starts_condition() const
    {
        int depth = 0;
        for (std::size_t j = i_; j < toks_.size(); ++j)
        {
            const TokenKind kind = toks_[j].kind;
            if (kind == TokenKind::LParen)
            {
                ++depth;
            }
            else if (kind == TokenKind::RParen)
            {
                --depth;
                if (depth == 0)
                    return false;
            }
            else if (kind == TokenKind::Newline || kind == TokenKind::Eof ||
                     kind == TokenKind::Then)
            {
                return false;
            }
            else if (depth == 1 && is_condition_operator(kind))
            {
                return true;
            }
        }
        return false;
    }

    ExprTree Parser::parse_cond_elem()
    {
        if (match(TokenKind::Not))
            return make_unary_node<NodeNot>(parse_cond_elem());

        if (check(TokenKind::LParen) && lparen_starts_condition())
        {
            advance(); // '('
            ExprTree inner = parse_cond_or();
            expect(TokenKind::RParen, "')' to close the parenthesised condition");
            return inner;
        }
        return parse_comparison();
    }

    ExprTree Parser::parse_comparison()
    {
        ExprTree lhs = parse_expr();
        const TokenKind kind = current().kind;

        ExprTree node;
        switch (kind)
        {
        case TokenKind::Assign:
            node = std::make_unique<NodeEqual>();
            break;
        case TokenKind::NotEqual:
            node = std::make_unique<NodeNotEqual>();
            break;
        case TokenKind::Less:
            node = std::make_unique<NodeInferior>();
            break;
        case TokenKind::LessEqual:
            node = std::make_unique<NodeInfEqual>();
            break;
        case TokenKind::Greater:
            node = std::make_unique<NodeSuperior>();
            break;
        case TokenKind::GreaterEqual:
            node = std::make_unique<NodeSupEqual>();
            break;
        default:
            fail("expected a comparison operator (=, !=, <, <=, >, >=)",
                 current());
        }
        advance();
        node->arguments.push_back(std::move(lhs));
        node->arguments.push_back(parse_expr());
        return node;
    }

    // ── expressions ──────────────────────────────────────────────────────────

    ExprTree Parser::parse_expr()
    {
        ExprTree left = parse_term();
        while (check(TokenKind::Plus) || check(TokenKind::Minus))
        {
            const bool plus = advance().kind == TokenKind::Plus;
            ExprTree right = parse_term();
            left = plus ? make_binary_node<NodeAdd>(std::move(left),
                                                    std::move(right))
                        : make_binary_node<NodeSub>(std::move(left),
                                                    std::move(right));
        }
        return left;
    }

    ExprTree Parser::parse_term()
    {
        ExprTree left = parse_factor();
        while (check(TokenKind::Star) || check(TokenKind::Slash))
        {
            const bool star = advance().kind == TokenKind::Star;
            ExprTree right = parse_factor();
            left = star ? make_binary_node<NodeMult>(std::move(left),
                                                     std::move(right))
                        : make_binary_node<NodeDiv>(std::move(left),
                                                    std::move(right));
        }
        return left;
    }

    ExprTree Parser::parse_factor()
    {
        ExprTree base = parse_unary();
        if (match(TokenKind::Caret))
            return make_binary_node<NodePow>(std::move(base), parse_factor());
        return base;
    }

    ExprTree Parser::parse_unary()
    {
        if (match(TokenKind::Minus))
            return make_unary_node<NodeUminus>(parse_atom());
        if (match(TokenKind::Plus))
            return make_unary_node<NodeUplus>(parse_atom());
        return parse_atom();
    }

    ExprTree Parser::parse_call(TokenKind function)
    {
        advance(); // the function name
        expect(TokenKind::LParen, "'(' after the function name");

        std::vector<ExprTree> args;
        if (!check(TokenKind::RParen))
        {
            args.push_back(parse_expr());
            while (match(TokenKind::Comma))
                args.push_back(parse_expr());
        }
        const Token &close =
            expect(TokenKind::RParen, "')' to close the argument list");

        const std::size_t n = args.size();
        ExprTree node;
        switch (function)
        {
        case TokenKind::Min:
            node = std::make_unique<NodeMin>();
            if (n != 2)
                fail("min() takes exactly two arguments", close);
            break;
        case TokenKind::Max:
            node = std::make_unique<NodeMax>();
            if (n != 2)
                fail("max() takes exactly two arguments", close);
            break;
        case TokenKind::Smooth:
            node = std::make_unique<NodeSmooth>();
            if (n != 2)
                fail("smooth() takes exactly two arguments (value, half-width)",
                     close);
            break;
        case TokenKind::Log:
            node = std::make_unique<NodeLog>();
            if (n != 1)
                fail("log() takes exactly one argument", close);
            break;
        case TokenKind::Exp:
            node = std::make_unique<NodeExp>();
            if (n != 1)
                fail("exp() takes exactly one argument", close);
            break;
        case TokenKind::Sqrt:
            node = std::make_unique<NodeSqrt>();
            if (n != 1)
                fail("sqrt() takes exactly one argument", close);
            break;
        case TokenKind::Abs:
            node = std::make_unique<NodeAbs>();
            if (n != 1)
                fail("abs() takes exactly one argument", close);
            break;
        default:
            fail("unknown function", close);
        }
        node->arguments = std::move(args);
        return node;
    }

    ExprTree Parser::parse_atom()
    {
        const Token &tok = current();
        switch (tok.kind)
        {
        case TokenKind::Number:
        {
            advance();
            auto node = std::make_unique<NodeConst>();
            try
            {
                node->value = std::stod(tok.lexeme);
            }
            catch (const std::exception &)
            {
                fail("number out of range: '" + tok.lexeme + "'", tok);
            }
            return node;
        }
        case TokenKind::Identifier:
        {
            advance();
            auto node = std::make_unique<NodeVar>();
            node->name = tok.lexeme;
            return node;
        }
        case TokenKind::Spot:
        {
            advance();
            expect(TokenKind::LParen, "'(' after 'spot'");
            expect(TokenKind::RParen, "')' — spot() takes no arguments in v1");
            return std::make_unique<NodeSpot>();
        }
        case TokenKind::Min:
        case TokenKind::Max:
        case TokenKind::Log:
        case TokenKind::Exp:
        case TokenKind::Sqrt:
        case TokenKind::Abs:
        case TokenKind::Smooth:
            return parse_call(tok.kind);
        case TokenKind::LParen:
        {
            advance();
            ExprTree inner = parse_expr();
            expect(TokenKind::RParen, "')' to close the expression");
            return inner;
        }
        default:
            fail("expected a number, a variable, a function call, or '('", tok);
        }
    }

    // ── free function ────────────────────────────────────────────────────────

    std::vector<Event> parse_script(const std::string &source)
    {
        Lexer lexer(source);
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens), source);
        return parser.parse();
    }

} // namespace quantModeling::scripting
