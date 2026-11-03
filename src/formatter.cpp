#include "quidra/formatter.hpp"

#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"

#include <algorithm>
#include <optional>
#include <set>
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
        case TokenKind::RealLiteral:
        case TokenKind::ImaginaryLiteral:
        case TokenKind::String:
        case TokenKind::KwClass:
        case TokenKind::KwPrivate:
        case TokenKind::KwPublic:
        case TokenKind::KwImport:
        case TokenKind::KwConst:
        case TokenKind::KwLocal:
        case TokenKind::KwReturn:
        case TokenKind::KwIf:
        case TokenKind::KwThen:
        case TokenKind::KwElif:
        case TokenKind::KwElse:
        case TokenKind::KwParallel:
        case TokenKind::KwWhile:
        case TokenKind::KwFor:
        case TokenKind::KwMatch:
        case TokenKind::KwTry:
        case TokenKind::KwBreak:
        case TokenKind::KwContinue:
        case TokenKind::KwThis:
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
        case TokenKind::RealLiteral:
        case TokenKind::ImaginaryLiteral:
        case TokenKind::String:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
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
        case TokenKind::Caret:
        case TokenKind::Pipe:
            return true;
        default:
            return false;
    }
}

// The tokens after which an operand starts. A '.' there begins a marked call
// (`x = .update()`, `return .f()`, `f(a, .g())`, `try .optimizer.update()`):
// it is the mutation marker (L1), not a member access, which always follows
// a complete operand.
bool operand_follows(TokenKind kind) {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::Comma:
        case TokenKind::LParen:
        case TokenKind::LBracket:
        case TokenKind::Colon:
        case TokenKind::Pipe:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Caret:
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::Less:
        case TokenKind::LessEq:
        case TokenKind::Greater:
        case TokenKind::GreaterEq:
        case TokenKind::KwReturn:
        case TokenKind::KwIf:
        case TokenKind::KwThen:
        case TokenKind::KwElif:
        case TokenKind::KwElse:
        case TokenKind::KwParallel:
        case TokenKind::KwWhile:
        case TokenKind::KwMatch:
        case TokenKind::KwTry:
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

// Where the mutation markers are: the offsets the parser accepted, when the
// source parses. A marker right after a type in a definition
// (`void .update()`, `T | error .save(...)`) is known only from the
// parse; without one, every '.' keeps a gap it has.
struct Markers {
    bool parsed{};
    std::set<std::size_t> offsets;
};

std::optional<std::string> canonical_gap(
    const std::vector<const Token*>& tokens, std::size_t index, const Markers& markers,
    std::string_view gap) {
    const auto previous=tokens[index-1]->kind;
    const auto current=tokens[index]->kind;

    // A run of three dots is a shape ellipsis, not a marked call or member
    // access. Keep the comma gap before it and join its dots.
    const bool ellipsis_start = current == TokenKind::Dot && index + 2 < tokens.size() &&
        tokens[index + 1]->kind == TokenKind::Dot && tokens[index + 2]->kind == TokenKind::Dot;
    if (ellipsis_start) return previous == TokenKind::Comma ? " " : "";
    if (previous == TokenKind::Dot && current == TokenKind::Dot) return "";
    if(current==TokenKind::Dot && previous!=TokenKind::KwThis){
        // One space before a marker, none after '(' or '['; a member
        // access takes none.
        const bool marker=markers.offsets.contains(tokens[index]->span.start.offset)||
                          operand_follows(previous);
        if(marker){
            if(previous==TokenKind::LParen||previous==TokenKind::LBracket) return "";
            if(previous==TokenKind::Minus) return std::nullopt;
            return " ";
        }
        if(!markers.parsed&&!gap.empty()) return std::nullopt;
        return "";
    }
    if(previous==TokenKind::Dot||current==TokenKind::Dot) return "";
    if(current==TokenKind::Semicolon) return "";
    if(previous==TokenKind::Semicolon) return " ";
    if(previous==TokenKind::LParen||previous==TokenKind::LBracket||
       previous==TokenKind::LBrace) return "";
    if(current==TokenKind::RParen||current==TokenKind::RBracket||
       current==TokenKind::RBrace||current==TokenKind::Comma) return "";
    if(previous==TokenKind::Comma) return " ";
    // A brace shape follows its element type directly (tensor<real32>{2, 3})
    // and is followed by a name like a closing bracket.
    if(current==TokenKind::LBrace&&previous==TokenKind::Greater) return "";
    if(previous==TokenKind::RBrace&&word_like(current)) return " ";

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
       current==TokenKind::KwAnd||
       current==TokenKind::KwOr) {
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
    // serve both generic/comparison and slice syntax. The formatter
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
    Markers markers;
    try {
        const auto program=Parser(scanned).parse();
        markers.parsed=true;
        markers.offsets.insert(program.mutation_markers.begin(),program.mutation_markers.end());
    } catch(const std::exception&) {
        markers.parsed=false;
    }

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
        const auto desired=canonical_gap(tokens,index,markers,gap);
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
