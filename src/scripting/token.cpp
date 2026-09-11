#include "quantModeling/scripting/token.hpp"

namespace quantModeling::scripting
{

    std::string_view to_string(TokenKind kind)
    {
        switch (kind)
        {
            case TokenKind::Number:
                return "number";
            case TokenKind::Identifier:
                return "identifier";
            case TokenKind::DateEvent:
                return "date";
            case TokenKind::If:
                return "'if'";
            case TokenKind::Then:
                return "'then'";
            case TokenKind::Else:
                return "'else'";
            case TokenKind::EndIf:
                return "'endIf'";
            case TokenKind::Pays:
                return "'pays'";
            case TokenKind::And:
                return "'and'";
            case TokenKind::Or:
                return "'or'";
            case TokenKind::Not:
                return "'not'";
            case TokenKind::Min:
                return "'min'";
            case TokenKind::Max:
                return "'max'";
            case TokenKind::Log:
                return "'log'";
            case TokenKind::Exp:
                return "'exp'";
            case TokenKind::Sqrt:
                return "'sqrt'";
            case TokenKind::Abs:
                return "'abs'";
            case TokenKind::Smooth:
                return "'smooth'";
            case TokenKind::Spot:
                return "'spot'";
            case TokenKind::Plus:
                return "'+'";
            case TokenKind::Minus:
                return "'-'";
            case TokenKind::Star:
                return "'*'";
            case TokenKind::Slash:
                return "'/'";
            case TokenKind::Caret:
                return "'^'";
            case TokenKind::Assign:
                return "'='";
            case TokenKind::NotEqual:
                return "'!='";
            case TokenKind::Less:
                return "'<'";
            case TokenKind::LessEqual:
                return "'<='";
            case TokenKind::Greater:
                return "'>'";
            case TokenKind::GreaterEqual:
                return "'>='";
            case TokenKind::LParen:
                return "'('";
            case TokenKind::RParen:
                return "')'";
            case TokenKind::Comma:
                return "','";
            case TokenKind::Newline:
                return "end of line";
            case TokenKind::Indent:
                return "indent";
            case TokenKind::Dedent:
                return "dedent";
            case TokenKind::Eof:
                return "end of script";
        }
        return "?";
    }

} // namespace quantModeling::scripting
