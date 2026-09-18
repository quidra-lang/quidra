#include "quidra/formatter.hpp"

#include "quidra/lexer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {
namespace {

bool source_token(TokenKind kind) {
    return kind != TokenKind::Eof &&
           kind != TokenKind::Newline &&
           kind != TokenKind::Indent &&
           kind != TokenKind::Dedent;
}

bool horizontal_space(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\r';
    });
}

bool word_like(TokenKind kind) {
    switch (kind) {
        case TokenKind::Identifier:
        case TokenKind::Integer:
        case TokenKind::Float:
        case TokenKind::String:
        case TokenKind::KwClass:
        case TokenKind::KwOverride:
        case TokenKind::KwImport:
        case TokenKind::KwSuper:
        case TokenKind::KwConst:
        case TokenKind::KwReturn:
        case TokenKind::KwIf:
        case TokenKind::KwElif:
        case TokenKind::KwElse:
        case TokenKind::KwWhile:
        case TokenKind::KwFor:
        case TokenKind::KwIn:
        case TokenKind::KwMatch:
        case TokenKind::KwTry:
        case TokenKind::KwBreak:
        case TokenKind::KwContinue:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwNot:
        case TokenKind::KwAnd:
        case TokenKind::KwOr:
        case TokenKind::KwBitNot:
        case TokenKind::KwBitAnd:
        case TokenKind::KwBitOr:
        case TokenKind::KwBitXor:
            return true;
        default:
            return false;
    }
}

bool expression_end(TokenKind kind) {
    switch (kind) {
        case TokenKind::Identifier:
        case TokenKind::Integer:
        case TokenKind::Float:
        case TokenKind::String:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwSuper:
        case TokenKind::RParen:
        case TokenKind::RBracket:
            return true;
        default:
            return false;
    }
}

bool spaced_operator(TokenKind kind) {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::LessEq:
        case TokenKind::GreaterEq:
        case TokenKind::Plus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Pipe:
            return true;
        default:
            return false;
    }
}

std::optional<std::string> canonical_gap(
    const std::vector<const Token*>& tokens, std::size_t index) {
    const auto previous=tokens[index-1]->kind;
    const auto current=tokens[index]->kind;

    if(previous==TokenKind::Dot||current==TokenKind::Dot) return "";
    if(previous==TokenKind::LParen||previous==TokenKind::LBracket) return "";
    if(current==TokenKind::RParen||current==TokenKind::RBracket||
       current==TokenKind::Comma) return "";
    if(previous==TokenKind::Comma) return " ";

    if(current==TokenKind::LParen &&
       (previous==TokenKind::Identifier||previous==TokenKind::RParen||
        previous==TokenKind::RBracket||previous==TokenKind::Greater)) {
        return "";
    }
    if(current==TokenKind::LBracket &&
       (previous==TokenKind::Identifier||previous==TokenKind::RParen||
        previous==TokenKind::RBracket||previous==TokenKind::Greater)) {
        return "";
    }

    if(previous==TokenKind::Ampersand) return "";
    if(current==TokenKind::Ampersand) {
        if(previous==TokenKind::LParen||previous==TokenKind::LBracket) return "";
        return " ";
    }

    if(spaced_operator(previous)||spaced_operator(current)) return " ";
    if(previous==TokenKind::KwAnd||previous==TokenKind::KwOr||
       previous==TokenKind::KwIn||current==TokenKind::KwAnd||
       current==TokenKind::KwOr||current==TokenKind::KwIn) {
        return " ";
    }

    if(current==TokenKind::Minus) return " ";
    if(previous==TokenKind::Minus) {
        const bool unary=index<2||!expression_end(tokens[index-2]->kind);
        return unary ? std::optional<std::string>{""}
                     : std::optional<std::string>{" "};
    }

    if(word_like(previous)&&word_like(current)) return " ";
    if(previous==TokenKind::RBracket&&word_like(current)) return " ";

    // '<', '>', and ':' are deliberately left untouched here: the same tokens
    // serve both generic/comparison and inheritance/slice syntax. The formatter
    // only canonicalizes spacing when the token role is unambiguous.
    return std::nullopt;
}

struct Edit {
    std::size_t start{};
    std::size_t end{};
    std::string replacement;
};

} // namespace

std::string format_source(std::string_view source) {
    const auto scanned=Lexer(source).scan();
    std::vector<const Token*> tokens;
    tokens.reserve(scanned.size());
    for(const auto& token:scanned)
        if(source_token(token.kind)) tokens.push_back(&token);

    std::vector<Edit> edits;
    for(std::size_t index=1;index<tokens.size();++index){
        const auto& previous=*tokens[index-1];
        const auto& current=*tokens[index];
        if(previous.span.end.line!=current.span.start.line) continue;
        if(previous.span.end.offset>current.span.start.offset||
           current.span.start.offset>source.size()) continue;
        const auto start=previous.span.end.offset;
        const auto end=current.span.start.offset;
        const auto gap=source.substr(start,end-start);
        if(!horizontal_space(gap)) continue;
        const auto desired=canonical_gap(tokens,index);
        if(!desired||gap==*desired) continue;
        edits.push_back(Edit{start,end,*desired});
    }

    std::string output;
    output.reserve(source.size()+16);
    std::size_t cursor=0;
    for(const auto& edit:edits){
        if(edit.start<cursor) continue;
        output.append(source.substr(cursor,edit.start-cursor));
        output+=edit.replacement;
        cursor=edit.end;
    }
    output.append(source.substr(cursor));
    if(!output.empty()&&output.back()!='\n') output.push_back('\n');
    return output;
}

} // namespace quidra
