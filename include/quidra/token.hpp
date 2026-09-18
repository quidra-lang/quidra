#pragma once
#include "quidra/diagnostic.hpp"
#include <string>

namespace quidra {

enum class TokenKind {
    Eof, Newline, Indent, Dedent, Pipe, Ampersand,
    Identifier, Integer, Float, String,
    KwClass, KwOverride, KwImport, KwSuper, KwConst,
    KwReturn, KwIf, KwElif, KwElse, KwWhile, KwFor, KwIn, KwMatch, KwTry,
    KwBreak, KwContinue,
    KwTrue, KwFalse, KwNot, KwAnd, KwOr,
    KwBitNot, KwBitAnd, KwBitOr, KwBitXor,
    LParen, RParen, LBracket, RBracket, Colon, Comma, Dot,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    Plus, Minus, Star, Slash, Percent,
    EqEq, NotEq, Less, LessEq, Greater, GreaterEq
};

struct Token {
    TokenKind kind{};
    std::string text;
    SourceSpan span{};
};

const char* token_name(TokenKind kind);

} // namespace quidra
