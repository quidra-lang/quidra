#include "quidra/lexer.hpp"
#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace quidra {
namespace {

std::size_t invalid_utf8_offset(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t width = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            width = 2;
            value = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            width = 3;
            value = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            width = 4;
            value = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return index;
        }

        if (index + width > text.size()) return index;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return index;
            value = (value << 6U) | (byte & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            return index;
        }
        index += width;
    }
    return std::string_view::npos;
}

SourcePos source_position_at(std::string_view text, std::size_t offset) {
    SourcePos position;
    for (std::size_t index = 0; index < offset; ++index) {
        ++position.offset;
        if (text[index] == '\n') {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }
    return position;
}

} // namespace

const char* token_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Indent: return "indent"; case TokenKind::Dedent: return "dedent"; case TokenKind::Pipe: return "|"; case TokenKind::Ampersand: return "&";
        case TokenKind::Eof: return "end of file"; case TokenKind::Newline: return "newline";
        case TokenKind::Identifier: return "identifier"; case TokenKind::Integer: return "integer";
        case TokenKind::Float: return "float"; case TokenKind::String: return "string";
        case TokenKind::KwClass: return "class"; case TokenKind::KwOverride: return "override"; case TokenKind::KwImport: return "import"; case TokenKind::KwSuper: return "super"; case TokenKind::KwConst: return "const";
        case TokenKind::KwReturn: return "return"; case TokenKind::KwIf: return "if";
        case TokenKind::KwElif: return "elif"; case TokenKind::KwElse: return "else";
        case TokenKind::KwWhile: return "while"; case TokenKind::KwFor: return "for";
        case TokenKind::KwIn: return "in"; case TokenKind::KwMatch: return "match";
        case TokenKind::KwTry: return "try"; case TokenKind::KwBreak: return "break";
        case TokenKind::KwContinue: return "continue"; case TokenKind::KwTrue: return "true";
        case TokenKind::KwFalse: return "false"; case TokenKind::KwNot: return "not";
        case TokenKind::KwAnd: return "and"; case TokenKind::KwOr: return "or";
        case TokenKind::LParen: return "("; case TokenKind::RParen: return ")";
        case TokenKind::LBracket: return "["; case TokenKind::RBracket: return "]";
        case TokenKind::Colon: return ":"; case TokenKind::Comma: return ","; case TokenKind::Dot: return ".";
        case TokenKind::Assign: return "=";
        case TokenKind::PlusAssign: return "+="; case TokenKind::MinusAssign: return "-=";
        case TokenKind::StarAssign: return "*="; case TokenKind::SlashAssign: return "/=";
        case TokenKind::PercentAssign: return "%="; case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-"; case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/"; case TokenKind::Percent: return "%";
        case TokenKind::EqEq: return "=="; case TokenKind::NotEq: return "!=";
        case TokenKind::Less: return "<"; case TokenKind::LessEq: return "<=";
        case TokenKind::Greater: return ">"; case TokenKind::GreaterEq: return ">=";
    }
    return "token";
}

char Lexer::peek(std::size_t lookahead) const {
    const auto i = index_ + lookahead;
    return i < source_.size() ? source_[i] : '\0';
}

char Lexer::advance() {
    const char c = source_[index_++];
    pos_.offset = index_;
    if (c == '\n') { ++pos_.line; pos_.column = 1; }
    else { ++pos_.column; }
    return c;
}

bool Lexer::match(char c) {
    if (eof() || peek() != c) return false;
    advance();
    return true;
}

void Lexer::skip_space_and_comments() {
    for (;;) {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) {
            if (peek() == '\t') error("INDENTATION", "Tabs are prohibited.", pos_);
            advance();
        }
        if (!eof() && peek() == '/' && peek(1) == '/') {
            while (!eof() && peek() != '\n') advance();
            continue;
        }
        break;
    }
}

Token Lexer::make(TokenKind kind, std::size_t start_index, SourcePos start, std::string text) {
    if (text.empty() && index_ > start_index) text = std::string(source_.substr(start_index, index_ - start_index));
    return Token{kind, std::move(text), SourceSpan{start, pos_}};
}

[[noreturn]] void Lexer::error(std::string code, std::string message, SourcePos start) const {
    throw CompileError(Diagnostic{std::move(code), std::move(message), SourceSpan{start, pos_}});
}

Token Lexer::identifier() {
    const auto start_index = index_;
    const auto start = pos_;
    advance();
    while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
    const auto text = std::string(source_.substr(start_index, index_ - start_index));
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"class", TokenKind::KwClass}, {"override", TokenKind::KwOverride}, {"import", TokenKind::KwImport}, {"super", TokenKind::KwSuper}, {"const", TokenKind::KwConst},
        {"return", TokenKind::KwReturn}, {"if", TokenKind::KwIf}, {"elif", TokenKind::KwElif}, {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile}, {"for", TokenKind::KwFor},
        {"in", TokenKind::KwIn}, {"match", TokenKind::KwMatch}, {"try", TokenKind::KwTry},
        {"break", TokenKind::KwBreak}, {"continue", TokenKind::KwContinue},
        {"true", TokenKind::KwTrue}, {"false", TokenKind::KwFalse}, {"not", TokenKind::KwNot},
        {"and", TokenKind::KwAnd}, {"or", TokenKind::KwOr}
    };
    if (const auto it = keywords.find(text); it != keywords.end()) return make(it->second, start_index, start, text);
    return make(TokenKind::Identifier, start_index, start, text);
}

Token Lexer::number() {
    const auto start_index = index_;
    const auto start = pos_;
    while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    bool floating = false;
    if (!eof() && peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        floating = true;
        advance();
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
        floating = true;
        advance();
        if (!eof() && (peek() == '+' || peek() == '-')) advance();
        if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) error("LEX_ERROR", "Malformed float exponent.", start);
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }
    return make(floating ? TokenKind::Float : TokenKind::Integer, start_index, start);
}

Token Lexer::string() {
    const auto start = pos_;
    advance();
    std::string value;
    int interpolation_depth = 0;
    bool interpolation_string = false;

    while (!eof()) {
        const char c = peek();
        if (c == '\0') error("LEX_ERROR", "NUL is not allowed in source strings.", start);

        if (c == '\r' && peek(1) == '\n') {
            advance();
            advance();
            value.push_back('\n');
            continue;
        }

        if (interpolation_depth == 0 && c == '"') {
            advance();
            return Token{TokenKind::String, std::move(value), SourceSpan{start, pos_}};
        }

        if (interpolation_depth == 0 && c == '{' && peek(1) == '{') {
            value.push_back(advance());
            value.push_back(advance());
            continue;
        }

        if (interpolation_depth == 0 && c == '{') {
            ++interpolation_depth;
            value.push_back(advance());
            continue;
        }

        if (interpolation_depth > 0) {
            if (interpolation_string) {
                if (c == '"') interpolation_string = false;
                value.push_back(advance());
                continue;
            }
            if (c == '"') {
                interpolation_string = true;
                value.push_back(advance());
                continue;
            }
            if (c == '{') {
                ++interpolation_depth;
                value.push_back(advance());
                continue;
            }
            if (c == '}') {
                --interpolation_depth;
                value.push_back(advance());
                continue;
            }
        }

        value.push_back(advance());
    }

    error("LEX_ERROR", "Unterminated string literal.", start);
}

std::vector<Token> Lexer::scan() {
    if (const auto invalid = invalid_utf8_offset(source_);
        invalid != std::string_view::npos) {
        const auto start = source_position_at(source_, invalid);
        auto end = start;
        ++end.offset;
        ++end.column;
        throw CompileError(Diagnostic{
            "INVALID_UTF8",
            "Quidra source must be valid UTF-8.",
            SourceSpan{start, end}});
    }

    std::vector<Token> tokens;
    while (!eof()) {
        skip_space_and_comments();
        if (eof()) break;
        const auto start_index = index_;
        const auto start = pos_;
        const char c = peek();
        if (c == '\0') error("LEX_ERROR", "NUL bytes are not allowed in Quidra source.", start);
        if (c == '\n') { advance(); tokens.push_back(make(TokenKind::Newline, start_index, start, "\n")); continue; }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { tokens.push_back(identifier()); continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) { tokens.push_back(number()); continue; }
        if (c == '"') { tokens.push_back(string()); continue; }
        advance();
        switch (c) {
            case '|': tokens.push_back(make(TokenKind::Pipe, start_index, start)); break;
            case '&': tokens.push_back(make(TokenKind::Ampersand, start_index, start)); break;
            case '(': tokens.push_back(make(TokenKind::LParen, start_index, start)); break;
            case ')': tokens.push_back(make(TokenKind::RParen, start_index, start)); break;
            case '[': tokens.push_back(make(TokenKind::LBracket, start_index, start)); break;
            case ']': tokens.push_back(make(TokenKind::RBracket, start_index, start)); break;
            case ':': tokens.push_back(make(TokenKind::Colon, start_index, start)); break;
            case ',': tokens.push_back(make(TokenKind::Comma, start_index, start)); break;
            case '.': tokens.push_back(make(TokenKind::Dot, start_index, start)); break;
            case '+': tokens.push_back(make(match('=') ? TokenKind::PlusAssign : TokenKind::Plus, start_index, start)); break;
            case '-': tokens.push_back(make(match('=') ? TokenKind::MinusAssign : TokenKind::Minus, start_index, start)); break;
            case '*': tokens.push_back(make(match('=') ? TokenKind::StarAssign : TokenKind::Star, start_index, start)); break;
            case '/': tokens.push_back(make(match('=') ? TokenKind::SlashAssign : TokenKind::Slash, start_index, start)); break;
            case '%': tokens.push_back(make(match('=') ? TokenKind::PercentAssign : TokenKind::Percent, start_index, start)); break;
            case '=': tokens.push_back(make(match('=') ? TokenKind::EqEq : TokenKind::Assign, start_index, start)); break;
            case '!':
                if (!match('=')) error("LEX_ERROR", "Expected '=' after '!'.", start);
                tokens.push_back(make(TokenKind::NotEq, start_index, start)); break;
            case '<': tokens.push_back(make(match('=') ? TokenKind::LessEq : TokenKind::Less, start_index, start)); break;
            case '>': tokens.push_back(make(match('=') ? TokenKind::GreaterEq : TokenKind::Greater, start_index, start)); break;
            default: error("LEX_ERROR", "Unexpected character in source.", start);
        }
    }
    tokens.push_back(Token{TokenKind::Eof, "", SourceSpan{pos_, pos_}});
    std::vector<Token> logical;
    std::vector<std::size_t> indents{0};
    int nesting = 0;
    bool line_start = true;
    for (const auto& t : tokens) {
        if (t.kind == TokenKind::Newline) {
            if (nesting == 0 && !line_start) { logical.push_back(t); line_start = true; }
            continue;
        }
        if(t.kind == TokenKind::Eof) {
            if (!line_start) logical.push_back(Token{TokenKind::Newline,"",t.span});
            while(indents.size()>1) { logical.push_back(Token{TokenKind::Dedent,"",t.span}); indents.pop_back(); }
            logical.push_back(t); break;
        }
        if(line_start && nesting==0) {
            auto indent=t.span.start.column-1;
            if(indent%4) error("INDENTATION","Indentation must use four spaces per level.",t.span.start);
            if(indent>indents.back()) {
                if(indent!=indents.back()+4) error("INDENTATION","Indentation cannot skip a level.",t.span.start);
                indents.push_back(indent); logical.push_back(Token{TokenKind::Indent,"",t.span});
            } else while(indent<indents.back()) { indents.pop_back(); logical.push_back(Token{TokenKind::Dedent,"",t.span}); }
            line_start=false;
        }
        if(t.kind==TokenKind::LParen || t.kind==TokenKind::LBracket) ++nesting;
        if(t.kind==TokenKind::RParen || t.kind==TokenKind::RBracket) --nesting;
        logical.push_back(t);
    }
    return logical;
}

} // namespace quidra