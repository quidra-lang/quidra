#pragma once
#include "quidra/diagnostic.hpp"
#include <string>

namespace quidra {

enum class TokenKind {
    Eof, Newline, Indent, Dedent, Pipe, Ampersand,
    Identifier, Integer, RealLiteral, ImaginaryLiteral, String,
    KwClass, KwEnum, KwPrivate, KwPublic, KwImport, KwConst,
    KwReturn, KwIf, KwThen, KwElif, KwElse, KwWhile, KwFor, KwMatch, KwTry,
    KwBreak, KwContinue, KwThis, KwParallel,
    KwTrue, KwFalse, KwNot, KwAnd, KwOr,
    KwBitNot, KwBitAnd, KwBitOr, KwBitXor,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace, Colon, Comma, Semicolon, Dot,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    Plus, Minus, Star, Slash, Percent, Caret, Hash,
    EqEq, NotEq, Less, LessEq, Greater, GreaterEq,
    KwLocal, DeclareAssign, Bang
};

struct Token {
    TokenKind kind{};
    std::string text;
    SourceSpan span{};
};

const char* token_name(TokenKind kind);

} // namespace quidra
