#include "quidra/parser.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include "class_initialization.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string_view>
#include <system_error>

namespace quidra {
namespace {

// Sets a parser flag for one nested construct and restores it afterwards.
class FlagScope {
public:
    FlagScope(bool& flag, bool value) : flag_(flag), saved_(flag) { flag_ = value; }
    ~FlagScope() { flag_ = saved_; }
    FlagScope(const FlagScope&) = delete;
    FlagScope& operator=(const FlagScope&) = delete;
private:
    bool& flag_;
    bool saved_;
};

constexpr const char* slice_marker_spacing_message =
    "A slice marker is written next to its colon: '<:', '>:', ':<' or ':>'.";
constexpr char braces_outside_shape_message[] = "Braces appear only in tensor shapes.";

void reject_reference_type(const Token& token) {
    if (token.kind == TokenKind::Ampersand)
        throw CompileError(Diagnostic{"PARSE_ERROR",
            "'&' is not part of a type: references are local variables and class fields, 'T &name'.",
            token.span});
}

void reject_reference_result(const Token& token) {
    if (token.kind == TokenKind::Ampersand)
        throw CompileError(Diagnostic{"PARSE_ERROR",
            "A function cannot return a reference; return a value, or keep the reference in a field.",
            token.span});
}

class ParseDepthGuard {
public:
    ParseDepthGuard(std::size_t& depth, std::size_t limit, const Token& token, const char* what)
        : depth_(depth) {
        ++depth_;
        if (depth_ > limit) {
            --depth_;
            throw CompileError(Diagnostic{"PARSE_DEPTH", std::string(what) + " nesting exceeds the parser limit.", token.span});
        }
    }
    ~ParseDepthGuard() { --depth_; }
private:
    std::size_t& depth_;
};

SourcePos relocated_position(const SourcePos& local, const SourcePos& base) {
    SourcePos result;
    result.offset = base.offset + local.offset;
    result.line = base.line + local.line - 1;
    result.column = local.line == 1 ? base.column + local.column - 1 : local.column;
    return result;
}

void relocate_span(SourceSpan& span, const SourcePos& base) {
    span.start = relocated_position(span.start, base);
    span.end = relocated_position(span.end, base);
}

// Skips a balanced group that starts at `index` with `open` and ends with
// its matching `close`; parentheses, brackets and braces nest inside it.
bool skip_balanced(const std::vector<Token>& tokens, std::size_t& index,
                   TokenKind open, TokenKind close) {
    if (index >= tokens.size() || tokens[index].kind != open) return false;
    std::vector<TokenKind> closers{close};
    ++index;
    while (index < tokens.size() && !closers.empty()) {
        const auto kind = tokens[index].kind;
        if (kind == TokenKind::Eof || kind == TokenKind::Newline) return false;
        if (kind == TokenKind::LParen) closers.push_back(TokenKind::RParen);
        else if (kind == TokenKind::LBracket) closers.push_back(TokenKind::RBracket);
        else if (kind == TokenKind::LBrace) closers.push_back(TokenKind::RBrace);
        else if (kind == TokenKind::RParen || kind == TokenKind::RBracket ||
                 kind == TokenKind::RBrace) {
            if (kind != closers.back()) return false;
            closers.pop_back();
        }
        ++index;
    }
    return closers.empty();
}

bool scan_type_lookahead(const std::vector<Token>& tokens, std::size_t& index,
                         std::size_t depth, std::size_t limit, bool call_scan=false) {
    if (tokens.empty()) return false;
    if (depth > limit) {
        const auto& token = tokens[std::min(index, tokens.size() - 1)];
        throw CompileError(Diagnostic{
            "PARSE_DEPTH",
            "Type nesting exceeds the parser limit.",
            token.span});
    }
    if(index>=tokens.size()) return false;
    std::string root_name;
    if(tokens[index].kind==TokenKind::LParen) {
        const auto begin=++index;
        if(!scan_type_lookahead(tokens,index,depth+1,limit,call_scan)) return false;
        bool comma=false;
        while(index<tokens.size() && tokens[index].kind==TokenKind::Comma) {
            comma=true;++index;
            if(!scan_type_lookahead(tokens,index,depth+1,limit,call_scan)) return false;
        }
        if(index>=tokens.size() || tokens[index].kind!=TokenKind::RParen) return false;
        bool union_group=false;std::size_t nesting=0;
        for(std::size_t i=begin;i<index;++i) {
            if(tokens[i].kind==TokenKind::LParen) ++nesting;
            else if(tokens[i].kind==TokenKind::RParen) --nesting;
            else if(!nesting && tokens[i].kind==TokenKind::Pipe) union_group=true;
        }
        if(call_scan && !comma && !union_group) return false;
        ++index;
    } else {
        if(tokens[index].kind!=TokenKind::Identifier || tokens[index].text=="_") return false;
        root_name=tokens[index++].text;
    }
    bool qualified = false;
    // A qualified name's '.' touches the name before it; a '.' after a gap
    // is a definition's mutation marker (`void .update()`).
    while (index < tokens.size() && tokens[index].kind == TokenKind::Dot &&
           tokens[index - 1].span.end.offset == tokens[index].span.start.offset) {
        qualified = true;
        ++index;
        if (index >= tokens.size() || tokens[index++].kind != TokenKind::Identifier) return false;
    }

    const auto scan_integer_expr = [&](TokenKind end_a, TokenKind end_b) -> bool {
        std::size_t parens = 0;
        bool expect_operand = true;
        bool any = false;
        while (index < tokens.size()) {
            const auto kind = tokens[index].kind;
            if (parens == 0 && (kind == end_a || kind == end_b)) break;
            if (expect_operand) {
                if (kind == TokenKind::Plus || kind == TokenKind::Minus) {
                    ++index;
                    continue;
                }
                if (kind == TokenKind::Integer) {
                    ++index;
                    any = true;
                    expect_operand = false;
                    continue;
                }
                if (kind == TokenKind::Identifier) {
                    ++index;
                    // A member chain and calls (`nat(n)`, `len(v)`,
                    // `v.size()`): an array extent is a whole expression.
                    while (index < tokens.size()) {
                        if (tokens[index].kind == TokenKind::Dot && index + 1 < tokens.size() &&
                            tokens[index + 1].kind == TokenKind::Identifier) {
                            index += 2;
                            continue;
                        }
                        if (tokens[index].kind == TokenKind::LParen) {
                            if (!skip_balanced(tokens, index, TokenKind::LParen, TokenKind::RParen))
                                return false;
                            continue;
                        }
                        break;
                    }
                    any = true;
                    expect_operand = false;
                    continue;
                }
                // `this.NAME`: a receiver field in an extent expression.
                if (kind == TokenKind::KwThis && index + 2 < tokens.size() &&
                    tokens[index + 1].kind == TokenKind::Dot &&
                    tokens[index + 2].kind == TokenKind::Identifier) {
                    index += 3;
                    any = true;
                    expect_operand = false;
                    continue;
                }
                if (kind == TokenKind::LParen) {
                    ++parens;
                    ++index;
                    continue;
                }
                return false;
            }
            if (kind == TokenKind::Plus || kind == TokenKind::Minus ||
                kind == TokenKind::Star || kind == TokenKind::Slash ||
                kind == TokenKind::Percent) {
                ++index;
                expect_operand = true;
                continue;
            }
            if (kind == TokenKind::RParen && parens > 0) {
                --parens;
                ++index;
                continue;
            }
            return false;
        }
        return any && !expect_operand && parens == 0;
    };

    const auto scan_shape = [&]() -> bool {
        if (index >= tokens.size() || tokens[index].kind != TokenKind::Less) return false;
        ++index;
        bool any = false;
        while (true) {
            if (index >= tokens.size()) return false;
            const bool wildcard =
                tokens[index].kind == TokenKind::Identifier &&
                tokens[index].text == "_" &&
                index + 1 < tokens.size() &&
                (tokens[index + 1].kind == TokenKind::Comma ||
                 tokens[index + 1].kind == TokenKind::Greater);
            if (wildcard) {
                ++index;
            } else if (!scan_integer_expr(TokenKind::Comma, TokenKind::Greater)) {
                return false;
            }
            any = true;
            if (index < tokens.size() && tokens[index].kind == TokenKind::Comma) {
                ++index;
                continue;
            }
            break;
        }
        return any && index < tokens.size() && tokens[index++].kind == TokenKind::Greater;
    };

    if (!qualified && root_name == "tensor" && index < tokens.size() &&
        tokens[index].kind == TokenKind::LBrace &&
        !skip_balanced(tokens, index, TokenKind::LBrace, TokenKind::RBrace)) return false;
    if (index < tokens.size() && tokens[index].kind == TokenKind::Less) {
        if (!qualified && (root_name == "func" || root_name == "fn")) {
            ++index;
            if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::Greater) return false;
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::LParen) return false;
            if (index < tokens.size() && tokens[index].kind != TokenKind::RParen) {
                if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
                while (index < tokens.size() && tokens[index].kind == TokenKind::Comma) {
                    ++index;
                    if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
                }
            }
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::RParen) return false;
        } else if (!qualified && root_name == "tensor") {
            ++index;
            if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::Greater) return false;
            if (index < tokens.size() && tokens[index].kind == TokenKind::Less &&
                !scan_shape()) return false;
            // A brace shape: the parser classifies its axes.
            if (index < tokens.size() && tokens[index].kind == TokenKind::LBrace &&
                !skip_balanced(tokens, index, TokenKind::LBrace, TokenKind::RBrace)) return false;
        } else {
            ++index;
            if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
            while (index < tokens.size() && tokens[index].kind == TokenKind::Comma) {
                ++index;
                if (!scan_type_lookahead(tokens, index, depth + 1, limit,call_scan)) return false;
            }
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::Greater) return false;
        }
    }
    // Recognize malformed nested reference types as declarations too, so
    // type_name can report the reference-specific diagnostic at '&'.
    const auto skip_nested_reference = [&]() {
        if (index < tokens.size() && tokens[index].kind == TokenKind::Ampersand &&
            (depth > 1 || call_scan ||
             (index + 1 < tokens.size() &&
              (tokens[index + 1].kind == TokenKind::LBracket ||
               tokens[index + 1].kind == TokenKind::Pipe)))) ++index;
    };
    skip_nested_reference();
    while (index < tokens.size() && tokens[index].kind == TokenKind::LBracket) {
        ++index;
        if (index < tokens.size() && tokens[index].kind != TokenKind::RBracket) {
            if (!scan_integer_expr(TokenKind::RBracket, TokenKind::RBracket)) return false;
        }
        if (index >= tokens.size() || tokens[index++].kind != TokenKind::RBracket) return false;
    }
    skip_nested_reference();
    if (index < tokens.size() && tokens[index].kind == TokenKind::Pipe) {
        ++index;
        return scan_type_lookahead(tokens, index, depth + 1, limit,call_scan);
    }
    return true;
}

void relocate_expr(Expr& expression, const SourcePos& base);

void relocate_type(TypeName& type, const SourcePos& base) {
    relocate_span(type.span, base);
    for (auto& argument : type.arguments) relocate_type(argument, base);
    for (auto& expression : type.dimension_expressions) {
        if (expression) relocate_expr(*expression, base);
    }
    for (auto& expression : type.tensor_shape_expressions) {
        if (expression) relocate_expr(*expression, base);
    }
    for (auto& axis : type.shape_axes) {
        relocate_span(axis.span, base);
        // A literal axis shares its expression with tensor_shape_expressions.
        if (axis.value && axis.kind != ShapeAxisKind::Unnamed) relocate_expr(*axis.value, base);
        else if (axis.value && !std::holds_alternative<IntegerExpr>(axis.value->data))
            relocate_expr(*axis.value, base);
    }
}

void relocate_arg(CallArg& argument, const SourcePos& base) {
    relocate_span(argument.span, base);
    relocate_expr(*argument.value, base);
}

void relocate_expr(Expr& expression, const SourcePos& base) {
    relocate_span(expression.span, base);
    if (auto* node = std::get_if<NameExpr>(&expression.data)) {
        if (node->this_qualifier) relocate_span(*node->this_qualifier, base);
    } else if (auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (auto& child : node->expressions) relocate_expr(*child, base);
    } else if (auto* node = std::get_if<TupleExpr>(&expression.data)) {
        for (auto& child : node->elements) relocate_expr(*child, base);
    } else if (auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        relocate_span(node->binding_span, base);
        if (node->type) relocate_type(*node->type, base);
        visit_array_comprehension(*node, [&](Expr& child) { relocate_expr(child, base); });
    } else if (auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (auto& child : node->elements) relocate_expr(*child, base);
    } else if (auto* node = std::get_if<IndexExpr>(&expression.data)) {
        relocate_expr(*node->base, base);
        for (auto& item : node->items) {
            relocate_span(item.span, base);
            if (item.index) relocate_expr(*item.index, base);
            if (item.start) relocate_expr(*item.start, base);
            if (item.stop) relocate_expr(*item.stop, base);
            if (item.step) relocate_expr(*item.step, base);
        }
    } else if (auto* node = std::get_if<MemberExpr>(&expression.data)) {
        relocate_expr(*node->base, base);
    } else if (auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        relocate_expr(*node->operand, base);
    } else if (auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        relocate_span(node->op_span, base);
        relocate_expr(*node->left, base);
        relocate_expr(*node->right, base);
    } else if (auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (auto& operand : node->operands) relocate_expr(*operand, base);
        for (auto& span : node->op_spans) relocate_span(span, base);
    } else if (auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (auto& argument : node->args) relocate_arg(argument, base);
        for (auto& argument : node->type_arguments) relocate_type(argument, base);
    } else if (auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        relocate_expr(*node->receiver, base);
        for (auto& argument : node->args) relocate_arg(argument, base);
        for (auto& argument : node->type_arguments) relocate_type(argument, base);
    } else if (auto* node = std::get_if<TryExpr>(&expression.data)) {
        relocate_expr(*node->value, base);
    } else if (auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (auto& condition : node->conditions) relocate_expr(*condition, base);
        for (auto& value : node->values) relocate_expr(*value, base);
        relocate_expr(*node->otherwise, base);
    }
}

SourcePos string_content_position(const Token& token, const std::size_t content_offset) {
    SourcePos position = token.span.start;
    ++position.offset;
    ++position.column;
    for (std::size_t i = 0; i < content_offset; ++i) {
        ++position.offset;
        if (token.text[i] == '\n') {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }
    return position;
}
} // namespace

const Token& Parser::peek(std::size_t n) const { return tokens_.at(std::min(current_ + n, tokens_.size() - 1)); }
const Token& Parser::previous() const { return tokens_.at(current_ - 1); }
bool Parser::at(TokenKind kind) const { return peek().kind == kind; }
bool Parser::match(TokenKind kind) { if (!at(kind)) return false; ++current_; return true; }

const Token& Parser::consume(TokenKind kind, const char* message) {
    if (at(kind)) return tokens_[current_++];
    if (at(TokenKind::LBrace) || at(TokenKind::RBrace)) error(peek(), braces_outside_shape_message);
    error(peek(), message);
}

[[noreturn]] void Parser::error(const Token& token, std::string message) const {
    throw CompileError(Diagnostic{"PARSE_ERROR", std::move(message), token.span});
}

void Parser::consume_newlines() { while (match(TokenKind::Newline)) {} }

void Parser::record(const CompileError& error) {
    if (diagnostics_.size() >= max_errors_) {
        throw CompileErrors(diagnostics_, true);
    }
    diagnostics_.push_back(error.diagnostic());
}

void Parser::synchronize_statement() {
    while (!at(TokenKind::Eof) && !at(TokenKind::Newline) && !at(TokenKind::Dedent)) {
        ++current_;
    }
    consume_newlines();
}

void Parser::end_statement(const char* context) {
    if (match(TokenKind::Newline)) { consume_newlines(); return; }
    if (at(TokenKind::Eof) || at(TokenKind::Dedent)) return;
    if (at(TokenKind::LBrace) || at(TokenKind::RBrace)) error(peek(), braces_outside_shape_message);
    error(peek(), std::string("Expected newline after ") + context + ".");
}

bool Parser::looks_like_declaration(bool function) const {
    std::size_t i = current_;
    if (!function && i < tokens_.size() && tokens_[i].kind == TokenKind::KwLocal) ++i;
    if (!function && i < tokens_.size() && tokens_[i].kind == TokenKind::KwConst) ++i;
    if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth)) return false;
    if (i < tokens_.size() && tokens_[i].kind == TokenKind::Ampersand) ++i;
    // A definition's mutation marker: `void .update()`. A statement such as
    // `counter .increment()` scans the same way, so a marked head counts as a
    // definition only when its parameter list is followed by a body or by
    // the prototype's ';'.
    bool marked = false;
    if (function && i < tokens_.size() && tokens_[i].kind == TokenKind::Dot) {
        marked = true;
        ++i;
    }
    if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;

    if (function && i < tokens_.size() && tokens_[i].kind == TokenKind::Less) {
        ++i;
        const auto scan_parameter = [&]() {
            if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Colon) {
                ++i;
                if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;
            }
            return true;
        };
        if (!scan_parameter()) return false;
        while (i < tokens_.size() && tokens_[i].kind == TokenKind::Comma) {
            ++i;
            if (!scan_parameter()) return false;
        }
        if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Greater) return false;
    }

    if (i >= tokens_.size()) return false;
    if (tokens_[i].kind != TokenKind::LParen) return !function;
    std::size_t depth = 0;
    for (; i < tokens_.size(); ++i) {
        if (tokens_[i].kind == TokenKind::LParen) ++depth;
        else if (tokens_[i].kind == TokenKind::RParen && --depth == 0) break;
        else if (tokens_[i].kind == TokenKind::Eof) return false;
    }
    if (++i >= tokens_.size()) return false;
    if (tokens_[i].kind == TokenKind::Semicolon) return function;
    if (tokens_[i].kind != TokenKind::Newline) return !function && !marked;
    while (i < tokens_.size() && tokens_[i].kind == TokenKind::Newline) ++i;
    const bool definition = i < tokens_.size() && tokens_[i].kind == TokenKind::Indent;
    return function ? definition : !definition;
}

bool Parser::looks_like_type_argument_call() const {
    if (!at(TokenKind::Less)) return false;

    std::size_t i = current_ + 1;
    if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth,true)) return false;
    while (i < tokens_.size() && tokens_[i].kind == TokenKind::Comma) {
        ++i;
        if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth,true)) return false;
    }
    return i + 1 < tokens_.size() &&
           tokens_[i].kind == TokenKind::Greater &&
           tokens_[i + 1].kind == TokenKind::LParen;
}

bool Parser::looks_like_cli_decl() const {
    return at(TokenKind::Identifier) && peek().text == "cli" &&
           peek(1).kind == TokenKind::Identifier &&
           peek(2).kind == TokenKind::Newline;
}

// `export` is contextual: it starts an export prefix only when a string
// literal follows it, so `export` stays an ordinary identifier elsewhere.
bool Parser::looks_like_export_prefix() const {
    return at(TokenKind::Identifier) && peek().text == "export" &&
           peek(1).kind == TokenKind::String;
}

Program Parser::parse() {
    Program p;
    consume_newlines();
    while (!at(TokenKind::Eof)) {
        try {
            if (at(TokenKind::KwImport)) p.imports.push_back(import_decl());
            else if (at(TokenKind::KwPublic)) {
                consume(TokenKind::KwPublic, "Expected public.");
                if (!at(TokenKind::KwImport)) error(peek(), "public is only valid as 'public import'.");
                auto declaration = import_decl();
                declaration.is_public = true;
                p.imports.push_back(std::move(declaration));
            }
            else if (at(TokenKind::KwPrivate)) private_declaration(p);
            else if (at(TokenKind::Identifier) && peek().text == "extern") p.functions.push_back(external_function_decl());
            else if (looks_like_export_prefix()) p.functions.push_back(export_function_decl());
            else if (looks_like_cli_decl()) cli_decl(p);
            else if (at(TokenKind::KwEnum)) p.enums.push_back(enum_decl());
            else if (at(TokenKind::KwClass)) p.classes.push_back(class_decl());
            else if (looks_like_declaration(true)) p.functions.push_back(function_decl());
            else if (at(TokenKind::KwIf) && peek(1).kind == TokenKind::Identifier &&
                     peek(1).text == "main" && peek(2).kind == TokenKind::Newline)
                p.statements.push_back(main_guard_stmt());
            else p.statements.push_back(statement());
        } catch (const CompileError& error) {
            record(error);
            synchronize_statement();
        }
        consume_newlines();
    }
    if (!diagnostics_.empty()) throw CompileErrors(std::move(diagnostics_));
    p.mutation_markers = std::move(mutation_markers_);
    std::sort(p.mutation_markers.begin(), p.mutation_markers.end());
    return p;
}

bool Parser::definition_marker() {
    if (!at(TokenKind::Dot)) return false;
    mutation_markers_.push_back(peek().span.start.offset);
    ++current_;
    return true;
}

// `private` before a top-level declaration (L3): a function or prototype,
// an extern declaration, a class, an enum or a `const` binding is then
// usable only inside its module. An exported C function is public, and
// `private` before anything else (an import, a cli block, a statement) is
// an error.
void Parser::private_declaration(Program& program) {
    const auto keyword = consume(TokenKind::KwPrivate, "Expected private.");
    if (looks_like_export_prefix()) {
        throw CompileError(Diagnostic{
            "FFI_EXPORT", "Exported C functions are public; remove 'private'.", keyword.span});
    }
    if (at(TokenKind::Identifier) && peek().text == "extern") {
        auto declaration = external_function_decl();
        declaration.is_private = true;
        program.functions.push_back(std::move(declaration));
    } else if (at(TokenKind::KwEnum)) {
        auto declaration = enum_decl();
        declaration.is_private = true;
        program.enums.push_back(std::move(declaration));
    } else if (at(TokenKind::KwClass)) {
        auto declaration = class_decl();
        declaration.is_private = true;
        program.classes.push_back(std::move(declaration));
    } else if (looks_like_declaration(true)) {
        auto declaration = function_decl();
        declaration.is_private = true;
        program.functions.push_back(std::move(declaration));
    } else if (at(TokenKind::KwConst) && looks_like_declaration(false)) {
        auto statement = binding_stmt();
        std::get<BindingStmt>(statement->data).is_private = true;
        program.statements.push_back(std::move(statement));
    } else {
        error(keyword, "private applies to declarations.");
    }
}

TypeName Parser::type_name() {
    ParseDepthGuard depth(type_depth_, max_parse_depth, peek(), "Type");
    TypeName t;
    const bool grouped=at(TokenKind::LParen);
    if(match(TokenKind::LParen)) {
        const auto start=previous().span.start;
        if(at(TokenKind::RParen)) throw CompileError(Diagnostic{"TUPLE","A tuple type has at least two elements.",peek().span});
        auto first=type_name();
        if(at(TokenKind::Ampersand)) {
            if(peek(1).kind == TokenKind::Comma)
                throw CompileError(Diagnostic{"TUPLE","Tuple types hold values, never references.",peek().span});
            reject_reference_type(peek());
        }
        if(match(TokenKind::Comma)) {
            t.name="$tuple";t.span.start=start;t.arguments.push_back(std::move(first));
            do {
                if(at(TokenKind::RParen)) throw CompileError(Diagnostic{"TUPLE","Trailing comma is not allowed in a tuple.",peek().span});
                t.arguments.push_back(type_name());
                if(at(TokenKind::Ampersand)) throw CompileError(Diagnostic{"TUPLE","Tuple types hold values, never references.",peek().span});
            } while(match(TokenKind::Comma));
        } else t=std::move(first);
        t.span.start=start;t.span.end=consume(TokenKind::RParen,"Expected ')' after tuple type.").span.end;
    } else {
        const auto first = consume(TokenKind::Identifier, "Expected an explicit type.");
        t.name = first.text;t.span = first.span;
    }

    // A '.' after a gap is a definition's mutation marker, not a part of
    // the qualified name (`void .update()`).
    while (at(TokenKind::Dot) && previous().span.end.offset == peek().span.start.offset) {
        ++current_;
        const auto part = consume(TokenKind::Identifier, "Expected type name after '.'.");
        t.name += "." + part.text;
        t.span.end = part.span.end;
    }

    const auto reject_old_tensor_shape = [&]() {
        const auto first_axis = peek();
        try {
            consume(TokenKind::Less, "Expected '<' before shape pattern.");
            if (!at(TokenKind::Greater)) {
                (void)type_integer_expression();
                while (match(TokenKind::Comma)) (void)type_integer_expression();
            }
            consume(TokenKind::Greater, "Expected '>' after shape pattern.");
        } catch (const CompileError&) {
            // Statement recovery still proceeds from the consumed old pattern.
        }
        throw CompileError(Diagnostic{"TENSOR_TYPE",
            "Tensor shapes are written in braces: write 'tensor<real32>{2, 3}'.",
            first_axis.span});
    };

    // A brace shape: each axis is classified on tokens before anything is
    // parsed, because the expression parser drops parentheses (`(n)` and
    // `n` would give the same NameExpr).
    const auto parse_brace_shape = [&]() {
        consume(TokenKind::LBrace, "Expected '{' before shape pattern.");
        t.brace_shape = true;
        if (at(TokenKind::RBrace)) error(peek(), "Shape pattern requires at least one axis.");
        const auto axis_end = [&](std::size_t n) {
            const auto kind = peek(n).kind;
            return kind == TokenKind::Comma || kind == TokenKind::RBrace;
        };
        while (true) {
            if (at(TokenKind::Dot)) {
                const auto ellipsis = peek();
                if (peek(1).kind != TokenKind::Dot || peek(2).kind != TokenKind::Dot)
                    error(ellipsis, "A shape ellipsis is written as '...'.");
                if (t.tensor_shape_ellipsis)
                    error(ellipsis, "A shape pattern may contain only one ellipsis.");
                current_ += 3;
                t.tensor_shape_ellipsis = t.tensor_shape_prefix.size();
                if (!axis_end(0)) error(peek(), "Expected ',' or '}' after shape ellipsis.");
                if (!match(TokenKind::Comma)) break;
                if (at(TokenKind::RBrace)) error(peek(), "Trailing comma is not allowed in a shape pattern.");
                continue;
            }
            ShapeAxis axis;
            axis.span = peek().span;
            long long code = -2;
            std::shared_ptr<Expr> literal;
            if (at(TokenKind::Identifier) && peek().text == "_" && axis_end(1)) {
                consume(TokenKind::Identifier, "Expected '_'.");
                axis.kind = ShapeAxisKind::Discard;
                code = -1;
            } else if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Identifier &&
                       (axis_end(2) || peek(2).kind == TokenKind::Assign)) {
                const auto type_token = consume(TokenKind::Identifier, "Expected 'nat'.");
                if (type_token.text != "nat") {
                    throw CompileError(Diagnostic{
                        "SHAPE_VARIABLE", "A shape variable is nat: write 'nat " +
                                              peek().text + "'.",
                        type_token.span});
                }
                const auto name = consume(TokenKind::Identifier, "Expected a shape variable name.");
                axis.name = name.text;
                axis.span.end = name.span.end;
                if (match(TokenKind::Assign)) {
                    axis.kind = ShapeAxisKind::DeclareEqual;
                    auto value = expression();
                    axis.span.end = value->span.end;
                    axis.value.reset(value.release());
                } else {
                    axis.kind = ShapeAxisKind::Declare;
                    code = -1;
                }
            } else if (at(TokenKind::Identifier) && axis_end(1)) {
                const auto name = consume(TokenKind::Identifier, "Expected an axis name.");
                axis.kind = ShapeAxisKind::Bare;
                axis.name = name.text;
                auto reference = std::make_shared<Expr>();
                reference->span = name.span;
                reference->data = NameExpr{name.text};
                axis.value = std::move(reference);
            } else {
                axis.kind = ShapeAxisKind::Unnamed;
                auto value = expression();
                axis.span.end = value->span.end;
                if (const auto* integer = std::get_if<IntegerExpr>(&value->data)) {
                    if (!integer->fits_u64 ||
                        integer->value >
                            static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
                        error(previous(), "Shape extent is too large.");
                    }
                    code = static_cast<long long>(integer->value);
                }
                axis.value.reset(value.release());
                if (code >= 0) literal = axis.value;
            }
            t.tensor_shape_prefix.push_back(code);
            t.tensor_shape_expressions.push_back(std::move(literal));
            t.shape_axes.push_back(std::move(axis));
            if (!match(TokenKind::Comma)) break;
            if (at(TokenKind::RBrace)) error(peek(), "Trailing comma is not allowed in a shape pattern.");
        }
        consume(TokenKind::RBrace, "Expected '}' after shape pattern.");
        if (!t.tensor_shape_ellipsis)
            t.tensor_rank = static_cast<long long>(t.tensor_shape_prefix.size());
        t.span.end = previous().span.end;
    };

    if (t.name == "tensor" && at(TokenKind::LBrace)) {
        throw CompileError(Diagnostic{
            "TENSOR_TYPE",
            "A tensor type names its element type: write 'tensor<real32>{2, 3}' or use 'auto'.",
            peek().span});
    }

    if (!grouped && t.name == "fn") t.name = "func";
    if (!grouped && t.name == "func") {
        consume(TokenKind::Less, "func requires '<Result>' before its parameter list.");
        if (at(TokenKind::Greater)) error(peek(), "func requires a result type.");
        t.arguments.push_back(type_name());
        reject_reference_type(peek());
        consume(TokenKind::Greater, "Expected '>' after func result type.");
        consume(TokenKind::LParen, "Expected '(' after func result type.");
        if (!at(TokenKind::RParen)) {
            do {
                t.function_parameters.push_back(type_name());
                reject_reference_type(peek());
            } while (match(TokenKind::Comma));
        }
        t.span.end = consume(TokenKind::RParen, "Expected ')' after func parameter types.").span.end;
    } else if (!grouped && at(TokenKind::Less)) {
        if (t.name == "tensor") {
            consume(TokenKind::Less, "Expected '<' before tensor element type.");
            if (at(TokenKind::Greater)) error(peek(), "tensor requires an element type.");
            t.arguments.push_back(type_name());
            reject_reference_type(peek());
            consume(TokenKind::Greater, "Expected '>' after tensor element type.");
            t.span.end = previous().span.end;
            if (at(TokenKind::Less)) reject_old_tensor_shape();
            else if (at(TokenKind::LBrace)) parse_brace_shape();
        } else {
            t.arguments = type_argument_list();
            t.span.end = previous().span.end;
        }
    }
    if (at(TokenKind::Ampersand) &&
        (peek(1).kind == TokenKind::LBracket || peek(1).kind == TokenKind::Pipe))
        reject_reference_type(peek());
    while (match(TokenKind::LBracket)) {
        long long length = -1;
        std::shared_ptr<Expr> dimension;
        if (!at(TokenKind::RBracket)) {
            // A whole expression (`int[nat(n)]`, `int[len(v)]`): the
            // brackets delimit it.
            auto parsed = expression();
            length = -2;
            if (const auto* literal = std::get_if<IntegerExpr>(&parsed->data)) {
                if (literal->value >
                    static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
                    error(previous(), "Array dimension is too large.");
                }
                length = static_cast<long long>(literal->value);
            }
            dimension.reset(parsed.release());
        }
        t.span.end = consume(TokenKind::RBracket, "Expected ']' after array dimension.").span.end;
        t.dimensions.push_back(length);
        t.dimension_expressions.push_back(std::move(dimension));
        ++t.array_depth;
    }

    if (at(TokenKind::Ampersand) && peek(1).kind == TokenKind::Pipe)
        reject_reference_type(peek());
    if (match(TokenKind::Pipe)) {
        TypeName u;
        u.name = "union";
        u.span.start = t.span.start;
        u.arguments.push_back(std::move(t));
        u.arguments.push_back(type_name());
        u.span.end = u.arguments.back().span.end;
        return u;
    }
    return t;
}

ExprPtr Parser::type_integer_factor() {
    if (match(TokenKind::Plus)) return type_integer_factor();

    if (match(TokenKind::Minus)) {
        const auto op = previous();
        auto operand = type_integer_factor();
        auto result = std::make_unique<Expr>();
        result->span = SourceSpan{op.span.start, operand->span.end};
        result->data = UnaryExpr{"-", std::move(operand)};
        return result;
    }

    if (match(TokenKind::Integer)) {
        const auto token = previous();
        std::uint64_t value{};
        const auto parsed =
            std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != token.text.data() + token.text.size()) {
            error(token, "Integer extent is too large.");
        }
        auto result = std::make_unique<Expr>();
        result->span = token.span;
        result->data = IntegerExpr{value, std::to_string(value), true};
        return result;
    }

    if (at(TokenKind::KwThis)) return this_field();

    if (match(TokenKind::Identifier)) {
        const auto token = previous();
        auto result = std::make_unique<Expr>();
        result->span = token.span;
        result->data = NameExpr{token.text};
        return result;
    }

    if (match(TokenKind::LParen)) {
        const auto start = previous().span.start;
        auto result = type_integer_expression();
        const auto end = consume(TokenKind::RParen, "Expected ')' after integer extent expression.").span.end;
        result->span = SourceSpan{start, end};
        return result;
    }

    error(peek(), "Expected an integer extent expression.");
}

ExprPtr Parser::type_integer_term() {
    auto left = type_integer_factor();
    while (at(TokenKind::Star) || at(TokenKind::Slash) || at(TokenKind::Percent)) {
        const auto op = tokens_[current_++];
        auto right = type_integer_factor();
        left = make_binary(std::move(left), op, std::move(right));
    }
    return left;
}

ExprPtr Parser::type_integer_expression() {
    auto left = type_integer_term();
    while (at(TokenKind::Plus) || at(TokenKind::Minus)) {
        const auto op = tokens_[current_++];
        auto right = type_integer_term();
        left = make_binary(std::move(left), op, std::move(right));
    }
    return left;
}


std::vector<TypeName> Parser::type_argument_list() {
    consume(TokenKind::Less, "Expected '<' before type arguments.");
    std::vector<TypeName> arguments;
    if (at(TokenKind::Greater)) error(peek(), "Generic type argument list cannot be empty.");
    do {
        arguments.push_back(type_name());
        reject_reference_type(peek());
    } while (match(TokenKind::Comma));
    consume(TokenKind::Greater, "Expected '>' after type arguments.");
    return arguments;
}

std::vector<std::string> Parser::type_parameter_list(std::vector<std::string>* constraints) {
    if (!match(TokenKind::Less)) return {};
    std::vector<std::string> parameters;
    std::vector<std::string> parsed_constraints;
    if (at(TokenKind::Greater)) error(peek(), "Generic type parameter list cannot be empty.");
    do {
        parameters.push_back(
            consume(TokenKind::Identifier, "Expected generic type parameter name.").text);
        std::string constraint;
        if (match(TokenKind::Colon)) {
            constraint =
                consume(TokenKind::Identifier, "Expected generic constraint name after ':'.").text;
        }
        parsed_constraints.push_back(std::move(constraint));
    } while (match(TokenKind::Comma));
    consume(TokenKind::Greater, "Expected '>' after generic type parameters.");
    if (constraints) *constraints = std::move(parsed_constraints);
    return parameters;
}

ImportDecl Parser::import_decl() {
    const auto start = consume(TokenKind::KwImport, "Expected import.").span.start;
    const auto first = consume(TokenKind::Identifier, "Expected module name or import alias.");

    ImportDecl result;
    result.alias = first.text;
    result.target = first.text;
    result.local_path = false;
    result.span.start = start;
    result.span.end = first.span.end;

    if (match(TokenKind::Assign)) {
        if (match(TokenKind::String)) {
            const auto target = previous();
            result.target = target.text;
            result.local_path = true;
            result.span.end = target.span.end;
        } else {
            const auto target = consume(TokenKind::Identifier, "Expected logical module name or local path string after '='.");
            result.target = target.text;
            result.span.end = target.span.end;
        }
    }

    end_statement("import");
    return result;
}

void Parser::cli_decl(Program& program) {
    const auto cli_token = consume(TokenKind::Identifier, "Expected cli.");
    const auto binding = consume(TokenKind::Identifier, "Expected CLI binding name.");
    for (const auto& existing : program.classes) {
        if (existing.name.rfind("$cli.", 0) == 0) error(cli_token, "Only one CLI declaration is allowed per source file.");
    }

    end_statement("cli declaration");
    consume(TokenKind::Indent, "Expected CLI fields indented by four spaces.");

    const std::string class_name = "$cli." + binding.text;
    ClassDecl generated;
    generated.name = class_name;
    generated.span = cli_token.span;
    generated.span.start.offset = std::numeric_limits<std::size_t>::max();
    generated.span.end.offset = std::numeric_limits<std::size_t>::max();

    std::vector<CallArg> constructor_args;
    std::size_t positional_index = 0;
    bool optional_positional_seen = false;
    consume_newlines();

    auto string_expr = [](const std::string& value, SourceSpan span) {
        auto out = std::make_unique<Expr>();
        out->span = span;
        out->data = StringExpr{value};
        return out;
    };
    auto integer_expr = [](std::size_t value, SourceSpan span) {
        auto out = std::make_unique<Expr>();
        out->span = span;
        out->data = IntegerExpr{static_cast<std::uint64_t>(value), std::to_string(value), true};
        return out;
    };

    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        const auto field_start = peek().span.start;
        auto type = type_name();
        if (at(TokenKind::Ampersand))
            throw CompileError(Diagnostic{"REFERENCE_BINDING",
                "A cli field holds a value from the command line; remove '&'.", peek().span});
        const auto field = consume(TokenKind::Identifier, "Expected CLI field name.");
        consume(TokenKind::Assign, "CLI fields require '= argument()', '= argument(default = ...)', '= option(default = ...)', or '= flag()'.");
        auto declaration = expression();
        auto* call = std::get_if<CallExpr>(&declaration->data);
        if (!call || (call->callee != "argument" && call->callee != "option" && call->callee != "flag")) {
            error(field, "CLI fields must use argument(), argument(default = ...), option(default = ...), or flag().");
        }

        auto hidden = std::make_unique<Expr>();
        hidden->span = declaration->span;
        std::vector<CallArg> hidden_args;
        hidden_args.push_back(CallArg{std::nullopt, false, string_expr(field.text, field.span), field.span});

        if (call->callee == "argument") {
            hidden_args.push_back(CallArg{std::nullopt, false, integer_expr(positional_index++, field.span), field.span});
            if (call->args.empty()) {
                if (optional_positional_seen) {
                    error(field, "Required positional arguments cannot follow an optional positional argument.");
                }
                hidden->data = CallExpr{"$std.cli.argument", std::move(hidden_args), {}};
            } else if (call->args.size() == 1 && call->args[0].name && *call->args[0].name == "default") {
                optional_positional_seen = true;
                hidden_args.push_back(CallArg{std::nullopt, false, std::move(call->args[0].value), call->args[0].span});
                hidden->data = CallExpr{"$std.cli.argument_optional", std::move(hidden_args), {}};
            } else {
                error(field, "argument() takes no arguments or exactly 'default = value'.");
            }
        } else if (call->callee == "option") {
            if (call->args.size() != 1 || !call->args[0].name || *call->args[0].name != "default") {
                error(field, "option() requires exactly 'default = value'.");
            }
            hidden_args.push_back(CallArg{std::nullopt, false, std::move(call->args[0].value), call->args[0].span});
            hidden->data = CallExpr{"$std.cli.option", std::move(hidden_args), {}};
        } else {
            if (!call->args.empty()) error(field, "flag() takes no arguments.");
            hidden->data = CallExpr{"$std.cli.flag", std::move(hidden_args), {}};
        }

        const auto field_span = SourceSpan{field_start, declaration->span.end};
        generated.fields.push_back(FieldDecl{field.text, std::move(type), field_span, nullptr});
        constructor_args.push_back(CallArg{field.text, false, std::move(hidden), field_span});
        end_statement("CLI field");
        consume_newlines();
    }

    const auto end = consume(TokenKind::Dedent, "Expected end of CLI declaration.").span.end;
    program.classes.push_back(std::move(generated));

    TypeName cli_type;
    cli_type.name = class_name;
    cli_type.span = SourceSpan{cli_token.span.start, binding.span.end};

    auto constructor = std::make_unique<Expr>();
    constructor->span = SourceSpan{cli_token.span.start, end};
    constructor->data = CallExpr{class_name, std::move(constructor_args), {}};

    auto binding_stmt = std::make_unique<Stmt>();
    binding_stmt->span = SourceSpan{cli_token.span.start, end};
    binding_stmt->data = BindingStmt{std::move(cli_type), binding.text, std::move(constructor), false, false, false, false, false, {}};
    program.statements.push_back(std::move(binding_stmt));

    auto finish_expr = std::make_unique<Expr>();
    finish_expr->span = cli_token.span;
    finish_expr->span.start.offset = std::numeric_limits<std::size_t>::max();
    finish_expr->span.end.offset = std::numeric_limits<std::size_t>::max();
    finish_expr->data = CallExpr{"$std.cli.finish", {}, {}};
    auto finish_stmt = std::make_unique<Stmt>();
    finish_stmt->span = finish_expr->span;
    finish_stmt->data = ExprStmt{std::move(finish_expr)};
    program.statements.push_back(std::move(finish_stmt));
}

EnumDecl Parser::enum_decl() {
    const auto start = consume(TokenKind::KwEnum, "Expected enum.").span.start;
    const auto name = consume(TokenKind::Identifier, "Expected enum name.");
    end_statement("enum declaration");
    consume(TokenKind::Indent, "Expected indented enum variants.");
    std::vector<EnumVariantDecl> variants;
    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        const auto variant = consume(TokenKind::Identifier, "Expected enum variant name.");
        std::optional<TypeName> payload;
        SourceSpan span = variant.span;
        if (match(TokenKind::LParen)) {
            payload = type_name();
            consume(TokenKind::RParen, "Expected ')' after enum variant payload type.");
            span.end = previous().span.end;
        }
        end_statement("enum variant");
        variants.push_back(EnumVariantDecl{variant.text, std::move(payload), span});
        consume_newlines();
    }
    if (variants.empty()) error(peek(), "enum requires at least one variant.");
    const auto end = consume(TokenKind::Dedent, "Expected end of enum body.").span.end;
    return EnumDecl{name.text, {}, std::move(variants), {start, end}};
}

ClassDecl Parser::class_decl() {
    const auto start = consume(TokenKind::KwClass, "Expected class.").span.start;
    const auto first_name = consume(TokenKind::Identifier, "Expected class name.");
    std::string name = first_name.text;
    while (match(TokenKind::Dot)) {
        const auto part = consume(TokenKind::Identifier, "Expected class name after '.'.");
        name += "." + part.text;
    }
    std::vector<std::string> type_constraints;
    auto type_parameters = type_parameter_list(&type_constraints);
    std::string self_name;
    std::optional<SourceSpan> self_span;
    if (at(TokenKind::Identifier)) {
        const auto instance = consume(TokenKind::Identifier, "Expected instance name.");
        self_name = instance.text;
        self_span = instance.span;
    }
    if (!match(TokenKind::LParen)) {
        throw CompileError(Diagnostic{"CONSTRUCTOR_SIGNATURE", "A class definition requires its argument list, including '()' for no arguments.", peek().span});
    }
    std::vector<Parameter> parameters;
    std::vector<FieldDecl> fields;
    while (!at(TokenKind::RParen)) {
        const bool local = match(TokenKind::KwLocal);
        const bool is_const = match(TokenKind::KwConst);
        auto type = type_name();
        const bool reference = match(TokenKind::Ampersand);
        reject_const_reference(is_const, reference);
        const auto parameter = consume(TokenKind::Identifier, "Expected class argument name.");
        ExprPtr value;
        if (match(TokenKind::Assign)) value = expression();
        if (!local) fields.push_back(FieldDecl{parameter.text, type, parameter.span, {}, is_const, false, reference});
        parameters.push_back(Parameter{parameter.text, std::move(type), reference, parameter.span, std::move(value), is_const, local});
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RParen, "Expected ')' after class arguments.");
    TypeName result;
    result.name = name;
    result.span = first_name.span;
    for (const auto& parameter : type_parameters) {
        TypeName argument;
        argument.name = parameter;
        argument.span = first_name.span;
        result.arguments.push_back(std::move(argument));
    }
    if (match(TokenKind::Pipe)) {
        const auto contract = consume(TokenKind::Identifier, "A class generation error contract must be '| error'.");
        if (contract.text != "error") error(contract, "A class generation error contract must be '| error'.");
        TypeName fallible;
        fallible.name = "union";
        fallible.span = {first_name.span.start, contract.span.end};
        fallible.arguments.push_back(std::move(result));
        TypeName error_type;
        error_type.name = "error";
        error_type.span = contract.span;
        fallible.arguments.push_back(std::move(error_type));
        result = std::move(fallible);
    }
    FunctionDecl initializer;
    initializer.name = "$initialize";
    initializer.parameters = std::move(parameters);
    initializer.return_type = std::move(result);
    initializer.is_constructor = true;
    std::vector<FunctionDecl> methods;
    auto end = previous().span.end;
    const bool prototype = match(TokenKind::Semicolon);
    if (prototype) end = previous().span.end;
    end_statement(prototype ? "class prototype" : "class signature");
    if (!prototype && match(TokenKind::Indent)) {
        consume_newlines();
        while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
            if (at(TokenKind::KwClass)) error(peek(), "Nested classes are prohibited.");
            if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
            if (at(TokenKind::KwPublic)) error(peek(), "public is only valid as 'public import' at top level.");
            const bool is_private = match(TokenKind::KwPrivate);
            std::optional<ForeignExport> foreign_export;
            if (looks_like_export_prefix()) foreign_export = foreign_export_prefix();
            if (looks_like_declaration(true)) {
                auto method = function_decl();
                method.is_private = is_private;
                method.foreign_export = std::move(foreign_export);
                methods.push_back(std::move(method));
            } else if (foreign_export) {
                error(peek(), "export \"C\" must precede a function declaration.");
            } else if (looks_like_declaration(false) && !at(TokenKind::KwLocal)) {
                const auto field_start = peek().span.start;
                const bool is_const = match(TokenKind::KwConst);
                auto type = type_name();
                const bool reference = match(TokenKind::Ampersand);
                reject_const_reference(is_const, reference);
                const auto field = consume(TokenKind::Identifier, "Expected member name.");
                ExprPtr value;
                bool reference_value = false;
                if (match(TokenKind::Assign)) {
                    reference_value = reference && match(TokenKind::Ampersand);
                    value = expression();
                    if (reference && !reference_value) throw CompileError(Diagnostic{"REFERENCE_BINDING", "Initialize a reference member with an explicit '&' storage reference.", field.span});
                }
                const auto field_end = value ? value->span.end : field.span.end;
                const SourceSpan span{field_start, field_end};
                end_statement("class member");
                fields.push_back(FieldDecl{field.text, std::move(type), span, {}, is_const, is_private, reference});
                if (value) {
                    auto statement = std::make_unique<Stmt>();
                    statement->span = span;
                    auto target = std::make_unique<Expr>();
                    target->span = field.span;
                    target->data = NameExpr{field.text, field.span};
                    if (reference) statement->data = RebindStmt{field.text, std::move(value), std::move(target)};
                    else statement->data = AssignStmt{std::move(target), std::move(value), {}, {}, nullptr};
                    initializer.body.push_back(std::move(statement));
                }
            } else {
                if (is_private) error(peek(), "private applies to class members and methods.");
                initializer.body.push_back(statement());
            }
            consume_newlines();
        }
        end = consume(TokenKind::Dedent, "Expected end of class body.").span.end;
    }
    initializer.span = {start, end};
    methods.insert(methods.begin(), std::move(initializer));
    ClassDecl declaration{name, {}, std::move(fields), std::move(methods), {start, end}, std::move(type_parameters), std::move(type_constraints)};
    declaration.self_name = std::move(self_name);
    declaration.self_span = self_span;
    declaration.is_prototype = prototype;
    elaborate_class_initialization(declaration);
    return declaration;
}

FunctionDecl Parser::function_decl() {
    const auto declaration_start = peek().span.start;
    auto result=type_name();auto start=declaration_start;
    reject_reference_result(peek());
    const bool marked=definition_marker();
    auto name=consume(TokenKind::Identifier,"Expected function name.");
    std::vector<std::string> type_constraints;
    auto type_parameters=type_parameter_list(&type_constraints);
    consume(TokenKind::LParen,"Expected '('."); std::vector<Parameter> params;
    while(!at(TokenKind::RParen)) {
        const bool is_local=match(TokenKind::KwLocal);
        const bool is_const=match(TokenKind::KwConst);
        auto type=type_name();bool write=match(TokenKind::Ampersand);reject_const_reference(is_const,write);auto p=consume(TokenKind::Identifier,"Expected parameter name.");
        ExprPtr value; if(match(TokenKind::Assign)) value=expression();
        params.push_back(Parameter{p.text,std::move(type),write,p.span,std::move(value),is_const,is_local});
        if(!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RParen,"Expected ')'.");
    if (match(TokenKind::Semicolon)) {
        auto end=previous().span.end;
        end_statement("function prototype");
        FunctionDecl declaration{name.text, {}, std::move(params), std::move(result), {},
                                 {start, end}, false, std::move(type_parameters), std::nullopt,
                                 std::move(type_constraints)};
        declaration.is_prototype = true;
        declaration.mutation_marker = marked;
        return declaration;
    }
    end_statement("function signature");auto body=block_until(false);
    auto end=previous().span.end;
    FunctionDecl declaration{name.text, {}, std::move(params), std::move(result), std::move(body),
                             {start, end}, false, std::move(type_parameters), std::nullopt,
                             std::move(type_constraints)};
    declaration.mutation_marker = marked;
    return declaration;
}


FunctionDecl Parser::external_function_decl() {
    const auto start=consume(TokenKind::Identifier,"Expected extern.").span.start;
    auto result=type_name();
    reject_reference_result(peek());
    const bool marked=definition_marker();
    const auto name=consume(TokenKind::Identifier,"Expected external function name.");
    std::vector<std::string> type_constraints;
    auto type_parameters=type_parameter_list(&type_constraints);
    consume(TokenKind::LParen,"Expected '(' after external function name.");
    std::vector<Parameter> parameters;
    while(!at(TokenKind::RParen)) {
        const bool is_local=match(TokenKind::KwLocal);
        const bool is_const=match(TokenKind::KwConst);
        auto type=type_name();
        const bool reference=match(TokenKind::Ampersand);
        reject_const_reference(is_const,reference);
        const auto parameter=consume(TokenKind::Identifier,"Expected external parameter name.");
        ExprPtr default_value;
        if(match(TokenKind::Assign)) default_value=expression();
        parameters.push_back(Parameter{
            parameter.text,std::move(type),reference,parameter.span,
            std::move(default_value),is_const,is_local});
        if(!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RParen,"Expected ')' after external parameters.");
    consume(TokenKind::Assign,"External function declaration requires '= \"c_symbol\"'.");
    const auto symbol=consume(TokenKind::String,"External function declaration requires a string C symbol.");
    end_statement("external function declaration");
    FunctionDecl declaration;
    declaration.name=name.text;
    declaration.parameters=std::move(parameters);
    declaration.return_type=std::move(result);
    declaration.span={start,symbol.span.end};
    declaration.type_parameters=std::move(type_parameters);
    declaration.type_constraints=std::move(type_constraints);
    declaration.external_symbol=symbol.text;
    declaration.mutation_marker=marked;
    return declaration;
}

ForeignExport Parser::foreign_export_prefix() {
    const auto keyword = consume(TokenKind::Identifier, "Expected export.");
    const auto abi = consume(TokenKind::String, "Expected the ABI string after export.");
    ForeignExport prefix;
    prefix.abi = abi.text;
    prefix.span = {keyword.span.start, abi.span.end};
    return prefix;
}

// `export "C"` before a top-level function prototype or definition. The
// frontend validates the ABI string and the target.
FunctionDecl Parser::export_function_decl() {
    auto prefix = foreign_export_prefix();
    if (!looks_like_declaration(true))
        error(peek(), "export \"C\" must precede a function declaration.");
    auto declaration = function_decl();
    declaration.span.start = prefix.span.start;
    declaration.foreign_export = std::move(prefix);
    return declaration;
}

std::vector<StmtPtr> Parser::block_until(bool) {
    consume(TokenKind::Indent, "Expected a block indented by four spaces.");
    std::vector<StmtPtr> body;
    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        try {
            if (at(TokenKind::KwClass)) error(peek(), "Nested classes are prohibited.");
            if (at(TokenKind::KwEnum)) error(peek(), "Nested enums are prohibited.");
            if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
            if (at(TokenKind::Identifier) && peek().text == "extern") error(peek(), "extern declarations are only valid at top level.");
            if (looks_like_export_prefix()) error(peek(), "export declarations are only valid at top level.");
            if (looks_like_declaration(true)) error(peek(), "Nested functions are prohibited.");
            body.push_back(statement());
        } catch (const CompileError& error) {
            record(error);
            synchronize_statement();
        }
        consume_newlines();
    }
    consume(TokenKind::Dedent, "Expected end of indented block.");
    return body;
}

StmtPtr Parser::statement() {
    ParseDepthGuard depth(statement_depth_, max_parse_statement_depth, peek(), "Statement");
    if (at(TokenKind::KwReturn)) return return_stmt();
    if (at(TokenKind::KwBreak) || at(TokenKind::KwContinue)) return loop_control_stmt();
    if (at(TokenKind::KwIf)) return if_stmt();
    if (at(TokenKind::KwParallel)) return parallel_stmt();
    if (at(TokenKind::KwWhile)) return while_stmt();
    if (at(TokenKind::KwFor)) return for_stmt();
    if (at(TokenKind::KwMatch)) return match_stmt();
    if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
    if (at(TokenKind::KwPublic)) error(peek(), "public is only valid as 'public import' at top level.");
    if (at(TokenKind::KwClass) || at(TokenKind::KwEnum))
        error(peek(),"class and enum are only valid at declaration boundaries.");
    if (looks_like_declaration(false)) return binding_stmt();
    if (at(TokenKind::KwLocal)) {
        const auto prefix = tokens_[current_++];
        if (!pattern_followed_by_assignment()) error(prefix, "local requires a declaration.");
        auto result = destructuring_stmt();
        auto* binding = std::get_if<BindingStmt>(&result->data);
        if (!binding) error(prefix, "local requires a declaration, not an assignment.");
        binding->is_local = true;
        result->span.start = prefix.span.start;
        return result;
    }
    if (pattern_followed_by_assignment()) return destructuring_stmt();
    if (at(TokenKind::Ampersand) &&
        ((peek(1).kind == TokenKind::Identifier &&
          peek(2).kind == TokenKind::Assign && peek(3).kind == TokenKind::Ampersand) ||
         (peek(1).kind == TokenKind::KwThis && peek(2).kind == TokenKind::Dot &&
          peek(3).kind == TokenKind::Identifier && peek(4).kind == TokenKind::Assign &&
          peek(5).kind == TokenKind::Ampersand)))
        return rebind_stmt();
    return expr_or_assign_stmt();
}

// D12: there are no read-only references. `const T &` is CONST_REFERENCE
// at the '&', in a parameter, an extern parameter and a binding.
void Parser::reject_const_reference(bool is_const, bool reference) const {
    if (!is_const || !reference) return;
    throw CompileError(Diagnostic{
        "CONST_REFERENCE",
        "Const references do not exist; write 'const T name' for a read-only value.",
        previous().span});
}

bool Parser::pattern_followed_by_assignment() const {
    if(!at(TokenKind::LParen)) return false;
    auto i=current_;std::vector<TokenKind> closes;bool comma=false;
    for(;i<tokens_.size();++i){const auto kind=tokens_[i].kind;
        if(kind==TokenKind::LParen)closes.push_back(TokenKind::RParen);
        else if(kind==TokenKind::LBracket)closes.push_back(TokenKind::RBracket);
        else if(kind==TokenKind::LBrace)closes.push_back(TokenKind::RBrace);
        else if(kind==TokenKind::RParen || kind==TokenKind::RBracket || kind==TokenKind::RBrace){if(closes.empty()||closes.back()!=kind)return false;closes.pop_back();if(closes.empty()){++i;break;}}
        else if(kind==TokenKind::Comma && closes.size()==1)comma=true;
        else if(kind==TokenKind::Eof || kind==TokenKind::Newline)return false;
    }
    if(!comma || i>=tokens_.size()) return false;
    auto next=tokens_[i].kind;
    return next==TokenKind::Assign || next==TokenKind::PlusAssign || next==TokenKind::MinusAssign ||
           next==TokenKind::StarAssign || next==TokenKind::SlashAssign || next==TokenKind::PercentAssign;
}

Pattern Parser::parse_pattern(bool match_case) {
    Pattern result;result.span.start=consume(TokenKind::LParen,"Expected tuple pattern.").span.start;
    for(;;) {
        if(at(TokenKind::RParen)) throw CompileError(Diagnostic{"TUPLE","Trailing comma is not allowed in a tuple.",peek().span});
        Pattern item;item.span.start=peek().span.start;
        if(at(TokenKind::Identifier) && peek().text=="_" &&
           (peek(1).kind==TokenKind::Comma || peek(1).kind==TokenKind::RParen)) {
            item.kind=Pattern::Kind::Discard;item.span=consume(TokenKind::Identifier,"Expected '_'.").span;
        } else {
            auto scan=current_;bool constant=tokens_[scan].kind==TokenKind::KwConst;if(constant)++scan;
            const bool typed=scan_type_lookahead(tokens_,scan,1,max_parse_depth) &&
                ((scan<tokens_.size() && tokens_[scan].kind==TokenKind::Identifier) ||
                 (scan+1<tokens_.size() && tokens_[scan].kind==TokenKind::Ampersand && tokens_[scan+1].kind==TokenKind::Identifier) ||
                 (match_case && scan<tokens_.size() && (tokens_[scan].kind==TokenKind::Comma || tokens_[scan].kind==TokenKind::RParen)));
            if(typed) {
                item.is_const=match(TokenKind::KwConst);item.type=type_name();item.reference=match(TokenKind::Ampersand);
                reject_const_reference(item.is_const,item.reference);
                if(at(TokenKind::Identifier)) {item.kind=Pattern::Kind::Declare;item.name=consume(TokenKind::Identifier,"Expected pattern binding name.").text;}
                else item.kind=Pattern::Kind::TypeOnly;
            } else if(at(TokenKind::LParen)) {
                auto group=current_;unsigned depth=0;bool comma=false;
                for(;group<tokens_.size();++group) {auto k=tokens_[group].kind;if(k==TokenKind::LParen)++depth;else if(k==TokenKind::RParen && --depth==0)break;else if(k==TokenKind::Comma && depth==1)comma=true;}
                if(comma) item=parse_pattern(match_case);else {item.kind=Pattern::Kind::Place;item.place=postfix();}
            } else {item.kind=Pattern::Kind::Place;item.place=postfix();}
            item.span.end=previous().span.end;
        }
        result.elements.push_back(std::move(item));
        if(!match(TokenKind::Comma))break;
    }
    result.span.end=consume(TokenKind::RParen,"Expected ')' after tuple pattern.").span.end;
    if(result.elements.size()<2) throw CompileError(Diagnostic{match_case?"MATCH_CASE":"TUPLE",match_case?"A case beginning with '(' that is not a type is a tuple pattern, which has at least two elements.":"A tuple pattern has at least two elements.",result.span});
    return result;
}

StmtPtr Parser::destructuring_stmt() {
    auto pattern=parse_pattern();const auto start=pattern.span.start;
    if(!match(TokenKind::Assign)) throw CompileError(Diagnostic{"TUPLE","A pattern is assigned with '='.",peek().span});
    const bool reference=match(TokenKind::Ampersand);auto value=expression();auto end=value->span.end;
    bool declares=false,places=false;visit_pattern(pattern,[&](const Pattern& p){declares|=p.kind==Pattern::Kind::Declare;places|=p.kind==Pattern::Kind::Place;});
    if(declares && places) throw CompileError(Diagnostic{"TUPLE","A pattern cannot mix declarations and existing places.",pattern.span});
    if(reference && !declares) throw CompileError(Diagnostic{"TUPLE","A reference pattern declares references only.",pattern.span});
    auto out=std::make_unique<Stmt>();out->span={start,end};
    if(declares) {BindingStmt binding;binding.declared_type.name="auto";binding.value=std::move(value);binding.reference=reference;binding.reference_initializer=reference;binding.pattern=std::move(pattern);out->data=std::move(binding);}
    else {AssignStmt assignment;assignment.target=std::make_unique<Expr>();assignment.target->data=TupleExpr{};assignment.value=std::move(value);assignment.pattern=std::move(pattern);out->data=std::move(assignment);}
    end_statement("destructuring");return out;
}

StmtPtr Parser::binding_stmt() {
    const auto start=peek().span.start;
    const bool is_local=match(TokenKind::KwLocal);
    const bool is_const=match(TokenKind::KwConst);
    // `const literal T NAME`: `literal` is a keyword only here, between
    // `const` and a type that a name follows; elsewhere it is an identifier.
    const bool literal_constant=is_const&&at(TokenKind::Identifier)&&peek().text=="literal"&&
        peek(1).kind==TokenKind::Identifier&&
        (peek(2).kind==TokenKind::Identifier||peek(2).kind==TokenKind::Less||
         peek(2).kind==TokenKind::LBracket);
    if(literal_constant)consume(TokenKind::Identifier,"Expected literal.");
    auto type=type_name();
    const bool reference=match(TokenKind::Ampersand);
    reject_const_reference(is_const, reference);
    auto name=consume(TokenKind::Identifier,"Expected binding name.");
    ExprPtr value; bool reference_initializer=false;
    if(match(TokenKind::Assign)) {
        reference_initializer=reference && match(TokenKind::Ampersand);
        value=expression();
    } else if (at(TokenKind::LParen)) {
        if (reference || is_language_type_name(type.name) || type.array_depth || type.brace_shape)
            error(peek(), "Direct initialization requires a class value binding; use '= expression' for other bindings.");
        auto args = call_arguments();
        value = std::make_unique<Expr>();
        value->span = SourceSpan{type.span.start, previous().span.end};
        const auto separator = type.name.rfind('.');
        if (separator == std::string::npos) {
            value->data = CallExpr{type.name, std::move(args), type.arguments};
        } else {
            auto receiver = std::make_unique<Expr>();
            receiver->span = type.span;
            const auto first = type.name.find('.');
            receiver->data = NameExpr{type.name.substr(0, first)};
            for (auto begin = first + 1; begin < separator;) {
                const auto end = type.name.find('.', begin);
                auto member = std::make_unique<Expr>();
                member->span = type.span;
                member->data = MemberExpr{std::move(receiver), type.name.substr(begin, end - begin)};
                receiver = std::move(member);
                begin = end + 1;
            }
            value->data = MethodCallExpr{std::move(receiver), type.name.substr(separator + 1),
                                        std::move(args), type.arguments};
        }
    }
    auto end=value?value->span.end:name.span.end;
    end_statement("binding");
    auto s=std::make_unique<Stmt>();s->span={start,end};
    s->data=BindingStmt{std::move(type),name.text,std::move(value),reference,reference_initializer,is_const,
                        literal_constant, false, {}, is_local};
    return s;
}

StmtPtr Parser::return_stmt() {
    auto token=consume(TokenKind::KwReturn,"Expected return.");ExprPtr value;
    if(at(TokenKind::Newline)||at(TokenKind::Eof)||at(TokenKind::Dedent)){value=std::make_unique<Expr>();value->span=token.span;value->data=VoidExpr{};}else value=expression();
    auto end=value->span.end;end_statement("return");auto s=std::make_unique<Stmt>();s->span={token.span.start,end};s->data=ReturnStmt{std::move(value)};return s;
}
StmtPtr Parser::loop_control_stmt() {
    const auto token = tokens_[current_++];
    end_statement(token.kind == TokenKind::KwContinue ? "continue" : "break");
    auto statement = std::make_unique<Stmt>();
    statement->span = token.span;
    statement->data = LoopControlStmt{token.kind == TokenKind::KwContinue};
    return statement;
}

StmtPtr Parser::if_stmt() {
    std::function<StmtPtr(TokenKind)> parse_branch = [&](TokenKind keyword) -> StmtPtr {
        const auto start = consume(
            keyword, keyword == TokenKind::KwIf ? "Expected if." : "Expected elif.").span.start;
        // A statement header is an or_expr, so an if-expression there is
        // written in parentheses.
        auto condition = or_expr();
        if (at(TokenKind::KwThen)) {
            throw CompileError(Diagnostic{
                "IF_EXPRESSION",
                "An if statement has no 'then'; its body follows on indented lines.",
                peek().span});
        }
        end_statement("condition");
        auto then_body = block_until(false);
        std::vector<StmtPtr> else_body;
        if (at(TokenKind::KwElif)) {
            // An elif chain recurses directly here, without passing through
            // statement(), so count it against the same budget. The guard goes
            // at the recursion site, not inside the lambda: an `if` head is
            // already counted by statement(), and counting it twice would put
            // the parser's limit below the checker's statement budget.
            ParseDepthGuard depth(
                statement_depth_, max_parse_statement_depth, peek(), "Statement");
            else_body.push_back(parse_branch(TokenKind::KwElif));
        } else if (match(TokenKind::KwElse)) {
            end_statement("else");
            else_body = block_until(false);
        }
        auto statement = std::make_unique<Stmt>();
        statement->span = {start, previous().span.end};
        statement->data = IfStmt{std::move(condition), std::move(then_body), std::move(else_body)};
        return statement;
    };
    return parse_branch(TokenKind::KwIf);
}

StmtPtr Parser::main_guard_stmt() {
    const auto start = consume(TokenKind::KwIf, "Expected if.").span.start;
    const auto main_name = consume(TokenKind::Identifier, "Expected main.");
    if (main_name.text != "main") error(main_name, "Direct-execution guard must be exactly 'if main'.");
    end_statement("main guard");
    auto body = block_until(false);
    if (at(TokenKind::KwElif) || at(TokenKind::KwElse)) {
        error(peek(), "'if main' does not have elif/else; imported modules never execute an import-only branch.");
    }
    auto statement = std::make_unique<Stmt>();
    statement->span = {start, previous().span.end};
    statement->data = MainGuardStmt{std::move(body), true, {}};
    return statement;
}
StmtPtr Parser::parallel_stmt() {
    const auto start=consume(TokenKind::KwParallel,"Expected parallel.").span.start;
    if(at(TokenKind::KwFor)) {
        auto statement=for_stmt();
        auto& loop=std::get<ForStmt>(statement->data);
        loop.parallel=true;
        if(loop.range && !loop.type && !loop.discards()) loop.declares=true;
        loop.parallel_span={start,statement->span.start};
        statement->span.start=start;
        return statement;
    }
    end_statement("parallel block");
    auto body=block_until(false);
    auto statement=std::make_unique<Stmt>();
    statement->span={start,previous().span.end};
    statement->data=ParallelStmt{std::move(body)};
    return statement;
}

StmtPtr Parser::while_stmt() {
    auto start=consume(TokenKind::KwWhile,"Expected while.").span.start;auto cond=or_expr();end_statement("condition");auto body=block_until(false);auto s=std::make_unique<Stmt>();s->span={start,previous().span.end};s->data=WhileStmt{std::move(cond),std::move(body)};return s;
}
bool Parser::at_collection_suffix() const {
    if(!at(TokenKind::LBracket)) return false;
    if(peek(1).kind==TokenKind::RBracket)
        return peek(2).kind!=TokenKind::LParen;
    auto cursor=current_+1;
    if(!scan_type_lookahead(tokens_,cursor,0,max_parse_depth)) return false;
    return cursor+2<tokens_.size() &&
        tokens_[cursor].kind==TokenKind::Identifier &&
        tokens_[cursor+1].kind==TokenKind::RBracket &&
        (tokens_[cursor+2].kind==TokenKind::Newline || tokens_[cursor+2].kind==TokenKind::Comma);
}

StmtPtr Parser::for_stmt() {
    const auto start=consume(TokenKind::KwFor,"Expected for.").span.start;
    const auto fail=[&](const char* code,const std::string& message) {
        throw CompileError(Diagnostic{code,message,peek().span});
    };
    ForStmt node;
    {
        node.legacy=false;
        auto cursor=current_;
        const bool typed=scan_type_lookahead(tokens_,cursor,0,max_parse_depth) &&
            ((cursor+1<tokens_.size() && tokens_[cursor].kind==TokenKind::Identifier &&
              tokens_[cursor+1].kind==TokenKind::Assign) ||
             (cursor+2<tokens_.size() && tokens_[cursor].kind==TokenKind::Ampersand &&
              tokens_[cursor+1].kind==TokenKind::Identifier &&
              tokens_[cursor+2].kind==TokenKind::Assign));
        const bool existing=(at(TokenKind::Identifier) && peek(1).kind==TokenKind::Assign) ||
            (at(TokenKind::Ampersand) && peek(1).kind==TokenKind::Identifier &&
             peek(2).kind==TokenKind::Assign);
        const bool pattern=!typed && pattern_followed_by_assignment();
        if(typed || existing || pattern) {
            node.declares=typed || pattern;
            if(typed) {
                auto type=type_name();
                node.is_auto=type.name=="auto" && type.dimensions.empty();
                if(!node.is_auto) node.type=std::move(type);
            }
            if(pattern){node.pattern=parse_pattern();bool declarations=false,places=false;visit_pattern(*node.pattern,[&](const Pattern& p){declarations|=p.kind==Pattern::Kind::Declare;places|=p.kind==Pattern::Kind::Place;node.writable|=p.reference;});if(declarations&&places)fail("TUPLE","A pattern cannot mix declarations and existing places.");node.declares=declarations;node.name="$for.pattern";}
            else {node.writable=match(TokenKind::Ampersand);node.name=consume(TokenKind::Identifier,"Expected iteration name.").text;}
            consume(TokenKind::Assign,"Expected '=' in collection loop.");
            const bool address=match(TokenKind::Ampersand);
            if(node.writable && node.name=="_")
                fail("DISCARD","'_' binds nothing; write '_' without '&'.");
            if(node.writable && !typed && !pattern)
                fail("FOR_HEADER","There is no existing-variable reference form: declare 'for T &x = &values[]'.");
            if(node.writable && !address)
                fail("FOR_HEADER","A reference loop iterates storage: write '&values[]'.");
            if(!node.writable && address)
                fail("FOR_HEADER","A value loop takes element values: remove '&'.");
            if(at(TokenKind::Dot))
                fail("FOR_HEADER","A marked call is iterated in parentheses: write 'for auto x = (.gen.batch())[]'.");
            {
                FlagScope header(collection_header_source_,true);
                node.iterable=postfix();
            }
            if(!at(TokenKind::LBracket)) {
                if(const auto* index=std::get_if<IndexExpr>(&node.iterable->data);
                   index && index->items.size()==1 && !index->items[0].slice) {
                    const auto* name=std::get_if<NameExpr>(&index->items[0].index->data);
                    if(name && name->name=="_")
                        fail("DISCARD","Write 'values[]' when the index is not used.");
                    fail("FOR_HEADER","Only a new nat index can be declared in '[]': write 'values[]' or 'values[nat i]'.");
                }
                fail("FOR_HEADER","A collection loop iterates elements: write 'values[]'.");
            }
            consume(TokenKind::LBracket,"Expected '['.");
            if(!at(TokenKind::RBracket)) {
                const auto type=type_name();
                const auto index=consume(TokenKind::Identifier,"Expected a new nat index name.");
                if(index.text=="_")
                    fail("DISCARD","Write 'values[]' when the index is not used.");
                if((type.name!="nat" && type.name!="auto") || !type.dimensions.empty())
                    fail("FOR_HEADER","The index of a collection loop is nat: write '[nat i]' and convert in the body, e.g. int(i).");
                node.index=ForIndexBinding{index.text,{type.span.start,index.span.end}};
            }
            consume(TokenKind::RBracket,"Expected ']' after collection suffix.");
            if(at(TokenKind::LBracket))
                fail("ELEMENT_ITERATION","One '[]' iterates one level; iterate 'row[]' in a nested for.");
            while(match(TokenKind::Comma)) {
                if(node.writable || node.pattern || node.index)
                    fail("FOR_HEADER","Multiple collections use array value bindings without patterns or indices.");
                ForCollectionBinding binding;
                const auto binding_start=peek().span.start;
                cursor=current_;
                binding.declares=scan_type_lookahead(tokens_,cursor,0,max_parse_depth) &&
                    cursor+1<tokens_.size() && tokens_[cursor].kind==TokenKind::Identifier &&
                    tokens_[cursor+1].kind==TokenKind::Assign;
                if(binding.declares) {
                    auto type=type_name();
                    binding.is_auto=type.name=="auto" && type.dimensions.empty();
                    if(!binding.is_auto) binding.type=std::move(type);
                }
                if(at(TokenKind::Ampersand) || at(TokenKind::LParen))
                    fail("FOR_HEADER","Multiple collections use array value bindings, not references or patterns.");
                binding.name=consume(TokenKind::Identifier,"Expected an array value binding.").text;
                if(!at(TokenKind::Assign)) fail("FOR_HEADER","Each collection binding requires '=' and an array followed by '[]'.");
                consume(TokenKind::Assign,"Expected '='.");
                if(at(TokenKind::Ampersand) || at(TokenKind::Dot))
                    fail("FOR_HEADER","Multiple collections take array values; parenthesize a marked call.");
                {
                    FlagScope header(collection_header_source_,true);
                    binding.iterable=postfix();
                }
                if(!at(TokenKind::LBracket) || peek(1).kind!=TokenKind::RBracket)
                    fail("FOR_HEADER","Each collection source must be an array followed by empty '[]'.");
                consume(TokenKind::LBracket,"Expected '['.");
                consume(TokenKind::RBracket,"Expected ']'.");
                if(at(TokenKind::LBracket))
                    fail("ELEMENT_ITERATION","One '[]' iterates one level; iterate 'row[]' in a nested for.");
                binding.span={binding_start,previous().span.end};
                node.collections.push_back(std::move(binding));
            }
        } else {
            const auto relation=[](TokenKind kind) {
                return kind==TokenKind::Less || kind==TokenKind::LessEq ||
                       kind==TokenKind::Greater || kind==TokenKind::GreaterEq;
            };
            const auto zero=[&](SourceSpan span) {
                auto value=std::make_unique<Expr>();
                value->span=span;
                value->data=IntegerExpr{0,"0",true};
                return value;
            };
            node.range.emplace();
            auto& range=*node.range;
            cursor=current_;
            const bool typed_left=scan_type_lookahead(tokens_,cursor,0,max_parse_depth) &&
                cursor+1<tokens_.size() && tokens_[cursor].kind==TokenKind::Identifier &&
                (tokens_[cursor+1].kind==TokenKind::Less || tokens_[cursor+1].kind==TokenKind::LessEq);
            if(typed_left) {
                node.type=type_name();
                if(node.type->name=="auto") {
                    if(at(TokenKind::Identifier)&&peek().text=="_")
                        fail("DISCARD","'_' takes no type; its type is inferred.");
                    fail("FOR_HEADER","'auto' is allowed only in collection loops.");
                }
                const auto variable=consume(TokenKind::Identifier,"Expected iteration name.");
                node.name=variable.text;
                node.declares=true;
                const auto op=peek().kind; ++current_;
                node.iterable=zero(variable.span);
                range.implicit_start_zero=true;
                range.start_inclusive=true;
                range.end_inclusive=op==TokenKind::LessEq;
                range.end=bit_or_expr();
            } else {
                node.iterable=bit_or_expr();
                if(!relation(peek().kind))
                    fail("FOR_HEADER","Expected '<', '<=', '>' or '>=' after the start of a for range.");
                const auto first=peek().kind; ++current_;
                const auto right_start=current_;
                range.start_inclusive=first==TokenKind::LessEq || first==TokenKind::GreaterEq;
                range.descending=first==TokenKind::Greater || first==TokenKind::GreaterEq;
                cursor=current_;
                const bool typed_right=scan_type_lookahead(tokens_,cursor,0,max_parse_depth) &&
                    cursor+1<tokens_.size() && tokens_[cursor].kind==TokenKind::Identifier &&
                    (relation(tokens_[cursor+1].kind) ||
                     (range.descending && (tokens_[cursor+1].kind==TokenKind::Newline ||
                      (tokens_[cursor+1].kind==TokenKind::Identifier && tokens_[cursor+1].text=="step"))));
                const bool full=typed_right?relation(tokens_[cursor+1].kind):
                    at(TokenKind::Identifier) && relation(peek(1).kind);
                if(full || range.descending) {
                    node.declares=typed_right;
                    if(typed_right) {
                        node.type=type_name();
                        if(node.type->name=="auto")
                            fail("FOR_HEADER","'auto' is allowed only in collection loops.");
                    }
                    if(!at(TokenKind::Identifier))
                        fail("FOR_HEADER","A for range needs a variable and an upper bound.");
                    const auto variable=consume(TokenKind::Identifier,"Expected loop variable.");
                    node.name=variable.text;
                    if(full) {
                        const auto second=peek().kind; ++current_;
                        if(range.descending!=(second==TokenKind::Greater || second==TokenKind::GreaterEq))
                            fail("FOR_HEADER","A for range keeps one direction: '<' and '<=' (ascending), or '>' and '>=' (descending).");
                        range.end_inclusive=second==TokenKind::LessEq || second==TokenKind::GreaterEq;
                        range.end=bit_or_expr();
                    } else {
                        // The omitted lower end is inclusive zero. This has
                        // exactly the semantics of START > [T] x >= 0.
                        range.end_inclusive=true;
                        range.end=zero(variable.span);
                        range.implicit_end_zero=true;
                    }
                } else {
                    const auto* variable=std::get_if<NameExpr>(&node.iterable->data);
                    if(!variable || variable->this_qualifier)
                        fail("FOR_HEADER","A for range needs a variable and an upper bound.");
                    node.name=variable->name;
                    node.declares=false;
                    const auto span=node.iterable->span;
                    current_=right_start;
                    range.end=bit_or_expr();
                    range.end_inclusive=first==TokenKind::LessEq;
                    range.start_inclusive=true;
                    node.iterable=zero(span);
                    range.implicit_start_zero=true;
                }
            }
            if(at(TokenKind::Identifier) && peek().text=="step") {
                ++current_; range.step=bit_or_expr();
            }
        }
    }
    if(at(TokenKind::Comma)) fail("FOR_HEADER","Multiple collections cannot be mixed with a range header.");
    end_statement("for header");
    node.body=block_until(false);
    auto statement=std::make_unique<Stmt>();
    statement->span={start,previous().span.end};
    statement->data=std::move(node);
    return statement;
}

StmtPtr Parser::match_stmt() {
    auto start=consume(TokenKind::KwMatch,"Expected match.").span.start;auto value=or_expr();end_statement("match value");consume(TokenKind::Indent,"Expected indented typed cases.");std::vector<MatchCase> cases;
    while(!at(TokenKind::Dedent)&&!at(TokenKind::Eof)) {
        if(at(TokenKind::LParen)) {
            auto scan=current_;const bool type_scan=scan_type_lookahead(tokens_,scan,1,max_parse_depth);auto group=current_;unsigned depth=0;for(;group<tokens_.size();++group){if(tokens_[group].kind==TokenKind::LParen)++depth;else if(tokens_[group].kind==TokenKind::RParen && --depth==0){++group;break;}}
            const auto follower=group<tokens_.size()?tokens_[group].kind:TokenKind::Eof;
            const bool type_case=type_scan && (follower==TokenKind::Identifier || follower==TokenKind::LBracket || follower==TokenKind::Pipe || follower==TokenKind::Newline);
            if(!type_case){auto pattern=parse_pattern(true);visit_pattern(pattern,[&](const Pattern& p){if(p.reference)throw CompileError(Diagnostic{"MATCH_CASE","Reference patterns are not used in match cases; bind the element by value.",p.span});if(p.kind==Pattern::Kind::Place)throw CompileError(Diagnostic{"MATCH_CASE","A match element is a type, a new binding, '_' or a nested pattern.",p.span});});if(at(TokenKind::Identifier))throw CompileError(Diagnostic{"MATCH_CASE","A tuple pattern binds its elements; to bind the whole tuple, write its type and a name, e.g. '(int, real64) p'.",peek().span});if(!at(TokenKind::Newline))throw CompileError(Diagnostic{"MATCH_CASE","A tuple pattern takes no suffix after ')'.",peek().span});auto span=pattern.span;end_statement("match case");auto body=block_until(false);MatchCase c;c.span=span;c.body=std::move(body);c.pattern=std::move(pattern);cases.push_back(std::move(c));continue;}
        }
        auto type=type_name();auto span=type.span;std::string tag;std::optional<std::string> binder;
        if(match(TokenKind::LParen)){
            tag=type.name;
            auto n=consume(TokenKind::Identifier,"Expected enum payload binder.");
            binder=n.text;span.end=n.span.end;
            consume(TokenKind::RParen,"Expected ')' after enum payload binder.");span.end=previous().span.end;
        }else if(at(TokenKind::Identifier)){auto n=consume(TokenKind::Identifier,"Expected binder.");binder=n.text;span.end=n.span.end;}
        end_statement("match case");auto body=block_until(false);
        cases.push_back(MatchCase{std::move(type),std::move(tag),std::move(binder),std::move(body),span,{}});
    }
    consume(TokenKind::Dedent,"Expected end of match.");auto result=std::make_unique<Stmt>();result->span={start,previous().span.end};result->data=MatchStmt{std::move(value),std::move(cases)};return result;
}

StmtPtr Parser::rebind_stmt() {
    const auto start=consume(TokenKind::Ampersand,"Expected '&'.").span.start;
    const bool field_binding = at(TokenKind::KwThis);
    const auto name=consume(field_binding ? TokenKind::KwThis : TokenKind::Identifier,
                            "Expected reference binding name after '&'.");
    ExprPtr place;
    if (field_binding) {
        consume(TokenKind::Dot,"Expected '.' after 'this'.");
        const auto field=consume(TokenKind::Identifier,"Expected reference field name after 'this.'.");
        place=std::make_unique<Expr>();
        place->span={name.span.start,field.span.end};
        place->data=NameExpr{field.text,name.span};
    }
    consume(TokenKind::Assign,"Expected '=' in address assignment.");
    consume(TokenKind::Ampersand,"Address assignment requires '&' on the right-hand side.");
    auto target=expression();
    const auto end=target->span.end;
    end_statement("address assignment");
    auto stmt=std::make_unique<Stmt>();stmt->span={start,end};
    stmt->data=RebindStmt{name.text,std::move(target),std::move(place)};
    return stmt;
}

StmtPtr Parser::expr_or_assign_stmt() {
    auto left = expression();
    const auto start = left->span.start;
    std::string compound_op;
    bool assignment = false;
    if (match(TokenKind::Assign)) {
        assignment = true;
    } else if (match(TokenKind::PlusAssign) || match(TokenKind::MinusAssign) ||
               match(TokenKind::StarAssign) || match(TokenKind::SlashAssign) ||
               match(TokenKind::PercentAssign)) {
        assignment = true;
        compound_op = previous().text.substr(0, 1);
    }
    if (assignment) {
        auto value = expression();
        const auto end = value->span.end;
        end_statement(compound_op.empty() ? "assignment" : "compound assignment");
        auto stmt = std::make_unique<Stmt>(); stmt->span = SourceSpan{start, end};
        stmt->data = AssignStmt{std::move(left), std::move(value), std::move(compound_op), {}, nullptr}; return stmt;
    }
    const auto span = left->span;
    end_statement("expression");
    auto stmt = std::make_unique<Stmt>(); stmt->span = span; stmt->data = ExprStmt{std::move(left)}; return stmt;
}

std::vector<CallArg> Parser::call_arguments(bool address_values) {
    FlagScope header_stop(collection_header_source_, false);
    FlagScope marker_stop(slice_start_marker_stop_, false);
    consume(TokenKind::LParen,"Expected '('.");
    std::vector<CallArg> args;
    if (!at(TokenKind::RParen)) {
        do {
            std::optional<std::string> label;
            bool writable = false;
            SourceSpan arg_span = peek().span;
            if (!address_values && at(TokenKind::Ampersand) &&
                peek(1).kind == TokenKind::Identifier &&
                peek(2).kind == TokenKind::Assign) {
                consume(TokenKind::Ampersand, "Expected '&'.");
                const auto& l = consume(TokenKind::Identifier, "Expected writable argument label.");
                consume(TokenKind::Assign, "Expected '=' after writable argument label.");
                consume(TokenKind::Ampersand, "Named writable arguments use '&name = &value'.");
                label = l.text;
                writable = true;
                arg_span = l.span;
            } else {
                if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Assign) {
                    const auto& l = consume(TokenKind::Identifier, "Expected argument label.");
                    consume(TokenKind::Assign, "Expected '=' after argument label.");
                    label = l.text;
                    arg_span = l.span;
                    if (!address_values && at(TokenKind::Ampersand)) {
                        error(peek(), "Named writable arguments use '&name = &value', not 'name = &value'.");
                    }
                }
                writable = !address_values && match(TokenKind::Ampersand);
            }
            auto value = expression();
            if (!label) arg_span = value->span;
            args.push_back(CallArg{std::move(label), writable, std::move(value), arg_span});
        } while (match(TokenKind::Comma) && !at(TokenKind::RParen));
    }
    consume(TokenKind::RParen,"Expected ')' after arguments.");
    return args;
}

ExprPtr Parser::expression() {
    ParseDepthGuard depth(expression_depth_, max_parse_depth, peek(), "Expression");
    if (at(TokenKind::KwIf)) return if_expression();
    return or_expr();
}

// An IF_EXPRESSION error that does not stop the parse: the if-expression is
// read on, so the diagnostics after it stay meaningful.
void Parser::report_if_expression(const Token& token, std::string message) {
    record(CompileError(Diagnostic{"IF_EXPRESSION", std::move(message), token.span}));
}

// if_expr = "if", or_expr, "then", or_expr,
//           { "elif", or_expr, "then", or_expr }, "else", or_expr
ExprPtr Parser::if_expression() {
    ParseDepthGuard depth(expression_depth_, max_parse_depth, peek(), "Expression");
    const auto start = consume(TokenKind::KwIf, "Expected if.").span.start;
    IfExpr node;
    for (;;) {
        // A condition or a then-branch ends at 'then', 'elif' or 'else',
        // never at a slice colon, so no slice marker belongs to it; the final
        // else branch keeps the item's marker rule.
        {
            FlagScope header_stop(collection_header_source_, false);
    FlagScope marker_stop(slice_start_marker_stop_, false);
            node.conditions.push_back(if_expression_part());
        }
        if (!match(TokenKind::KwThen)) {
            throw CompileError(Diagnostic{
                "IF_EXPRESSION", "An if-expression continues with 'then' after its condition.",
                peek().span});
        }
        {
            FlagScope header_stop(collection_header_source_, false);
    FlagScope marker_stop(slice_start_marker_stop_, false);
            node.values.push_back(if_expression_part());
        }
        if (match(TokenKind::KwElif)) continue;
        if (at(TokenKind::KwElse) && peek(1).kind == TokenKind::KwIf) {
            report_if_expression(peek(1), "'else if' is not used in an if-expression; write 'elif'.");
            current_ += 2;
            continue;
        }
        break;
    }
    if (!match(TokenKind::KwElse)) {
        throw CompileError(Diagnostic{
            "IF_EXPRESSION", "An if-expression needs 'else': every path must give a value.",
            peek().span});
    }
    node.otherwise = if_expression_part();
    auto result = std::make_unique<Expr>();
    result->span = SourceSpan{start, node.otherwise->span.end};
    result->data = std::move(node);
    return result;
}

// A condition or a branch of an if-expression: an or_expr, so an
// if-expression nested there is written in parentheses.
ExprPtr Parser::if_expression_part() {
    if (at(TokenKind::KwIf)) {
        report_if_expression(peek(), "A nested if-expression must be parenthesized.");
        return if_expression();
    }
    return or_expr();
}

ExprPtr Parser::inline_expression() {
    consume_newlines();
    auto result = expression();
    consume_newlines();
    if (!at(TokenKind::Eof)) error(peek(), "Unexpected token inside string interpolation.");
    if (!diagnostics_.empty()) throw CompileErrors(std::move(diagnostics_));
    return result;
}

ExprPtr Parser::string_expression(const Token& token) {
    std::vector<std::string> literals;
    std::vector<ExprPtr> expressions;
    std::vector<InterpolationFormat> formats;
    std::string literal;

    auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return std::string_view{};
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    };
    auto parse_count = [&](std::string_view text, const char* name, bool allow_zero) -> std::uint32_t {
        text = trim(text);
        if (text.empty()) error(token, std::string("Interpolation format '") + name + "' requires an integer value.");
        std::uint32_t value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            error(token, std::string("Interpolation format '") + name + "' requires a nonnegative integer literal.");
        if ((!allow_zero && value == 0) || value > 1000)
            error(token, std::string("Interpolation format '") + name + "' must be between " +
                         (allow_zero ? "0" : "1") + " and 1000.");
        return value;
    };
    auto parse_format = [&](std::string_view source) {
        InterpolationFormat format;
        source = trim(source);
        if (source.empty()) error(token, "Interpolation format cannot be empty.");
        std::size_t start = 0;
        while (start <= source.size()) {
            const auto comma = source.find(',', start);
            auto item = trim(source.substr(start, comma == std::string_view::npos ? source.size() - start : comma - start));
            if (item.empty()) error(token, "Interpolation format contains an empty option.");
            if (item == "zero") {
                if (format.zero) error(token, "Interpolation format 'zero' cannot be repeated.");
                format.zero = true;
            } else {
                const auto equal = item.find('=');
                if (equal == std::string_view::npos || item.find('=', equal + 1) != std::string_view::npos)
                    error(token, "Interpolation format options are int=N, frac=N, sig=N, or zero.");
                const auto name = trim(item.substr(0, equal));
                const auto value = item.substr(equal + 1);
                if (name == "int") {
                    if (format.integer_width) error(token, "Interpolation format 'int' cannot be repeated.");
                    format.integer_width = parse_count(value, "int", false);
                } else if (name == "frac") {
                    if (format.fractional_digits) error(token, "Interpolation format 'frac' cannot be repeated.");
                    format.fractional_digits = parse_count(value, "frac", true);
                } else if (name == "sig") {
                    if (format.significant_digits) error(token, "Interpolation format 'sig' cannot be repeated.");
                    format.significant_digits = parse_count(value, "sig", false);
                } else {
                    error(token, "Unknown interpolation format option '" + std::string(name) + "'.");
                }
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        if (format.fractional_digits && format.significant_digits)
            error(token, "Interpolation formats 'frac' and 'sig' cannot be used together.");
        if (format.zero && !format.integer_width)
            error(token, "Interpolation format 'zero' requires 'int'.");
        return format;
    };

    for (std::size_t i = 0; i < token.text.size();) {
        if (token.text[i] == '{') {
            if (i + 1 < token.text.size() && token.text[i + 1] == '{') { literal.push_back('{'); i += 2; continue; }
            literals.push_back(std::move(literal)); literal.clear();
            const auto expression_start = i + 1;
            // Braces nest too: a braced type argument
            // ("{first<tensor<real32>{2, 3}>(xs)}") is part of the expression.
            std::size_t j = expression_start; int paren = 0, bracket = 0, brace = 0; bool in_string = false;
            for (; j < token.text.size(); ++j) {
                const char c = token.text[j];
                if (in_string) { if (c=='"') in_string=false; continue; }
                if (c=='"') { in_string=true; continue; }
                if (c=='(') ++paren; else if (c==')') --paren; else if (c=='[') ++bracket; else if (c==']') --bracket;
                else if (c=='{') ++brace;
                else if (c=='}' && brace > 0) --brace;
                else if (c=='}' && paren==0 && bracket==0) break;
            }
            if (j >= token.text.size()) error(token, "Unclosed '{' in string interpolation.");

            const auto inner = std::string_view(token.text).substr(expression_start, j-expression_start);
            std::size_t colon = std::string_view::npos;
            paren = 0; bracket = 0; brace = 0; in_string = false;
            for (std::size_t k = 0; k < inner.size(); ++k) {
                const char c = inner[k];
                if (in_string) { if (c=='"') in_string=false; continue; }
                if (c=='"') { in_string=true; continue; }
                if (c=='(') ++paren; else if (c==')') --paren; else if (c=='[') ++bracket; else if (c==']') --bracket;
                else if (c=='{') ++brace; else if (c=='}') --brace;
                else if (c==':' && paren==0 && bracket==0 && brace==0) { colon = k; break; }
            }
            auto expression_text = colon == std::string_view::npos ? inner : inner.substr(0, colon);
            if (trim(expression_text).empty()) error(token, "String interpolation cannot be empty.");
            // `{&name}` is an input target: where scan("...") stores what it
            // reads. It is an ordinary storage expression behind the `&`.
            const auto ampersand = expression_text.find('&');
            const bool scan_target =
                ampersand != std::string_view::npos && trim(expression_text.substr(0, ampersand)).empty();
            if (scan_target && colon != std::string_view::npos) {
                error(token, "An input target {&name} takes no format.");
            }
            std::size_t nested_start = expression_start;
            if (scan_target) {
                nested_start = expression_start + ampersand + 1;
                expression_text = expression_text.substr(ampersand + 1);
                if (trim(expression_text).empty()) error(token, "An input target is written {&name}.");
            }
            const auto base = string_content_position(token, nested_start);
            try {
                Parser nested(Lexer(std::string(expression_text)).scan(), max_errors_);
                auto nested_expression = nested.inline_expression();
                relocate_expr(*nested_expression, base);
                if (scan_target) {
                    auto target = std::make_unique<Expr>();
                    target->span = nested_expression->span;
                    target->data = UnaryExpr{"scan&", std::move(nested_expression)};
                    nested_expression = std::move(target);
                }
                expressions.push_back(std::move(nested_expression));
            } catch (const CompileError& nested_error) {
                auto diagnostic = nested_error.diagnostic();
                relocate_span(diagnostic.span, base);
                throw CompileError(std::move(diagnostic));
            } catch (const CompileErrors& nested_errors) {
                auto diagnostics = nested_errors.diagnostics();
                for (auto& diagnostic : diagnostics) relocate_span(diagnostic.span, base);
                throw CompileErrors(std::move(diagnostics), nested_errors.truncated());
            }
            formats.push_back(colon == std::string_view::npos ? InterpolationFormat{} : parse_format(inner.substr(colon + 1)));
            i = j + 1; continue;
        }
        if (token.text[i] == '}') {
            if (i + 1 < token.text.size() && token.text[i+1] == '}') { literal.push_back('}'); i += 2; continue; }
            error(token, "Unmatched '}' in string interpolation. Use '}}' for a literal brace.");
        }
        literal.push_back(token.text[i++]);
    }
    literals.push_back(std::move(literal));
    auto result = std::make_unique<Expr>(); result->span = token.span;
    if (expressions.empty()) result->data = StringExpr{std::move(literals.front())};
    else result->data = StringTemplateExpr{std::move(literals), std::move(expressions), std::move(formats)};
    return result;
}

ExprPtr Parser::make_binary(ExprPtr left, const Token& op, ExprPtr right) {
    auto expr = std::make_unique<Expr>(); expr->span = SourceSpan{left->span.start, right->span.end};
    expr->data = BinaryExpr{op.text, std::move(left), std::move(right), op.span}; return expr;
}
ExprPtr Parser::or_expr() { auto e=and_expr(); while(match(TokenKind::KwOr)){auto op=previous(); e=make_binary(std::move(e),op,and_expr());} return e; }
ExprPtr Parser::and_expr() { auto e=equality(); while(match(TokenKind::KwAnd)){auto op=previous(); e=make_binary(std::move(e),op,equality());} return e; }
// The equality level (L14). One '==' or '!=' between operands that are not
// relational comparisons gives a BinaryExpr, as before. Two or more, or one
// next to an unparenthesized relational comparison, give one
// ComparisonChainExpr whose operators come from both levels, so that
// `a == b == c` is a chain and the checker sees and rejects `a < b == c`
// and every '!=' chain instead of reading them by precedence.
ExprPtr Parser::equality() {
    auto first = comparison();
    if (!at(TokenKind::EqEq) && !at(TokenKind::NotEq)) return first;
    struct Item {
        ExprPtr expression;
        bool relational{};
        std::vector<SourceSpan> op_spans;
    };
    std::vector<Item> items;
    items.push_back(Item{std::move(first), comparison_relational_, comparison_op_spans_});
    std::vector<Token> operators;
    while (match(TokenKind::EqEq) || match(TokenKind::NotEq)) {
        operators.push_back(previous());
        auto operand = comparison();
        items.push_back(Item{std::move(operand), comparison_relational_, comparison_op_spans_});
    }
    comparison_relational_ = false;
    comparison_op_spans_.clear();
    const bool relational = std::any_of(items.begin(), items.end(),
                                        [](const Item& item) { return item.relational; });
    if (operators.size() == 1 && !relational)
        return make_binary(std::move(items[0].expression), operators[0],
                           std::move(items[1].expression));
    ComparisonChainExpr chain;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            chain.ops.push_back(operators[i - 1].text);
            chain.op_spans.push_back(operators[i - 1].span);
        }
        auto& item = items[i];
        if (!item.relational) {
            chain.operands.push_back(std::move(item.expression));
        } else if (auto* binary = std::get_if<BinaryExpr>(&item.expression->data)) {
            chain.operands.push_back(std::move(binary->left));
            chain.ops.push_back(binary->op);
            chain.op_spans.push_back(item.op_spans.empty() ? binary->op_span : item.op_spans[0]);
            chain.operands.push_back(std::move(binary->right));
        } else if (auto* inner = std::get_if<ComparisonChainExpr>(&item.expression->data)) {
            for (auto& operand : inner->operands) chain.operands.push_back(std::move(operand));
            for (auto& op : inner->ops) chain.ops.push_back(std::move(op));
            for (const auto& span : inner->op_spans) chain.op_spans.push_back(span);
        } else {
            chain.operands.push_back(std::move(item.expression));
        }
    }
    auto e = std::make_unique<Expr>();
    e->span = SourceSpan{chain.operands.front()->span.start, chain.operands.back()->span.end};
    e->data = std::move(chain);
    return e;
}
// A '<' or '>' whose end touches the ':' that follows it.
bool Parser::at_marker_touching_colon() const {
    return (at(TokenKind::Less) || at(TokenKind::Greater)) &&
           peek(1).kind == TokenKind::Colon &&
           peek().span.end.offset == peek(1).span.start.offset;
}

// The relational level (L14). One operator gives a BinaryExpr, as before;
// two or more give a ComparisonChainExpr over the operands in order. The
// direction rule is the checker's, so inspect still sees the node.
ExprPtr Parser::comparison() {
    auto e = bit_or_expr();
    std::vector<ExprPtr> operands;
    std::vector<Token> operators;
    for (;;) {
        if (slice_start_marker_stop_ && (at(TokenKind::Less) || at(TokenKind::Greater)) &&
            peek(1).kind == TokenKind::Colon) {
            // A slice marker of the item being parsed: the item parser
            // takes it with its colon. Separated from the colon, it cannot
            // be a comparison either (':' starts no operand).
            if (at_marker_touching_colon()) break;
            error(peek(), slice_marker_spacing_message);
        }
        if (!(match(TokenKind::Less) || match(TokenKind::LessEq) ||
              match(TokenKind::Greater) || match(TokenKind::GreaterEq))) break;
        operators.push_back(previous());
        if (operands.empty()) operands.push_back(std::move(e));
        operands.push_back(bit_or_expr());
    }
    comparison_relational_ = !operators.empty();
    comparison_op_spans_.clear();
    for (const auto& op : operators) comparison_op_spans_.push_back(op.span);
    if (operators.empty()) return e;
    if (operators.size() == 1)
        return make_binary(std::move(operands[0]), operators[0], std::move(operands[1]));
    ComparisonChainExpr chain;
    for (const auto& op : operators) {
        chain.ops.push_back(op.text);
        chain.op_spans.push_back(op.span);
    }
    chain.operands = std::move(operands);
    auto chained = std::make_unique<Expr>();
    chained->span = SourceSpan{chain.operands.front()->span.start, chain.operands.back()->span.end};
    chained->data = std::move(chain);
    return chained;
}
ExprPtr Parser::bit_or_expr() { auto e=bit_xor_expr(); while(match(TokenKind::KwBitOr)){auto op=previous(); e=make_binary(std::move(e),op,bit_xor_expr());} return e; }
ExprPtr Parser::bit_xor_expr() { auto e=bit_and_expr(); while(match(TokenKind::KwBitXor)){auto op=previous(); e=make_binary(std::move(e),op,bit_and_expr());} return e; }
ExprPtr Parser::bit_and_expr() { auto e=shift_expr(); while(match(TokenKind::KwBitAnd)){auto op=previous(); e=make_binary(std::move(e),op,shift_expr());} return e; }
ExprPtr Parser::shift_expr() {
    auto e=term();
    for (;;) {
        if (at(TokenKind::Less) && peek(1).kind==TokenKind::Less) {
            Token op=peek();
            ++current_;
            const auto& second=consume(TokenKind::Less,"Expected second '<' in shift operator.");
            op.text="<<";
            op.span.end=second.span.end;
            e=make_binary(std::move(e),op,term());
            continue;
        }
        if (at(TokenKind::Greater) && peek(1).kind==TokenKind::Greater) {
            Token op=peek();
            ++current_;
            const auto& second=consume(TokenKind::Greater,"Expected second '>' in shift operator.");
            op.text=">>";
            op.span.end=second.span.end;
            e=make_binary(std::move(e),op,term());
            continue;
        }
        return e;
    }
}
ExprPtr Parser::term() { auto e=factor(); while(match(TokenKind::Plus)||match(TokenKind::Minus)){auto op=previous(); e=make_binary(std::move(e),op,factor());} return e; }
ExprPtr Parser::factor() { auto e=unary(); while(match(TokenKind::Star)||match(TokenKind::Slash)||match(TokenKind::Percent)){auto op=previous(); e=make_binary(std::move(e),op,unary());} return e; }

ExprPtr Parser::unary() {
    ParseDepthGuard depth(unary_depth_, max_parse_depth, peek(), "Unary expression");
    if (match(TokenKind::KwTry)) {
        const auto start = previous().span.start;
        auto value = unary();
        auto e=std::make_unique<Expr>(); e->span=SourceSpan{start,value->span.end}; e->data=TryExpr{std::move(value)}; return e;
    }
    if (match(TokenKind::Ampersand)) {
        const auto op=previous(); auto operand=unary(); auto e=std::make_unique<Expr>();
        e->span=SourceSpan{op.span.start,operand->span.end}; e->data=UnaryExpr{"&",std::move(operand)}; return e;
    }
    if (match(TokenKind::KwNot) || match(TokenKind::KwBitNot) || match(TokenKind::Minus)) {
        const auto op=previous(); auto operand=unary(); auto e=std::make_unique<Expr>();
        e->span=SourceSpan{op.span.start,operand->span.end}; e->data=UnaryExpr{op.text,std::move(operand)}; return e;
    }
    return power();
}

ExprPtr Parser::power() {
    auto e = at(TokenKind::Dot) ? marked_call() : postfix();
    if (match(TokenKind::Caret)) {
        const auto op = previous();
        e = make_binary(std::move(e), op, unary());
    }
    return e;
}

// `.f()`, `.obj.f()`, `.mod.T(...)`: the mutation marker before a postfix
// chain (L1). A '.' where an operand starts is a marker, never a member
// access, which always follows a complete operand. The chain must end in a
// call; the marker is recorded on that outermost call and covers the calls
// of the chain, not the calls nested in its arguments. A marked call is not
// a postfix, so it takes no further suffix.
ExprPtr Parser::marked_call() {
    const auto marker = consume(TokenKind::Dot, "Expected '.'.");
    auto e = postfix();
    if (auto* call = std::get_if<CallExpr>(&e->data)) {
        call->mutation_marker = true;
    } else if (auto* method = std::get_if<MethodCallExpr>(&e->data)) {
        method->mutation_marker = true;
    } else {
        error(marker, "'.' marks a call.");
    }
    mutation_markers_.push_back(marker.span.start.offset);
    e->span.start = marker.span.start;
    return e;
}

// In a free expression a bounded colon range materializes an integer
// array, while an indexing colon keeps the existing direct-slice AST.
// The internal call is generated only by this parser and is not a public API.
ExprPtr Parser::range_from(ExprPtr begin_value, SourcePos begin) {
    bool start_exclusive = false;
    bool end_exclusive = false;
    bool ascending_marker = false;
    bool descending_marker = false;
    if (begin_value && at_marker_touching_colon()) {
        start_exclusive = true;
        if (match(TokenKind::Less)) ascending_marker = true;
        else {
            consume(TokenKind::Greater, "Expected range marker.");
            descending_marker = true;
        }
    }
    const auto colon = consume(TokenKind::Colon, "Expected ':' in bounded range.");
    if (at(TokenKind::Less) || at(TokenKind::Greater)) {
        if (peek().span.start.offset != colon.span.end.offset)
            throw CompileError(Diagnostic{
                "SLICE_STEP", slice_marker_spacing_message, peek().span});
        end_exclusive = true;
        if (match(TokenKind::Less)) ascending_marker = true;
        else {
            consume(TokenKind::Greater, "Expected range marker.");
            descending_marker = true;
        }
    }
    if (ascending_marker && descending_marker)
        throw CompileError(Diagnostic{
            "SLICE_STEP", "Range endpoint markers must point in the same direction.",
            colon.span});
    const auto missing_endpoint = [&] {
        return at(TokenKind::Colon) || at(TokenKind::Comma) ||
               at(TokenKind::Newline) || at(TokenKind::Eof) ||
               at(TokenKind::RParen) || at(TokenKind::RBracket);
    };
    if (missing_endpoint())
        throw CompileError(Diagnostic{
            "SLICE_BOUNDS", "A standalone range requires an end value; only an indexed slice may omit it.",
            colon.span});
    ExprPtr end_value;
    {
        FlagScope no_nested_range(slice_start_marker_stop_, true);
        end_value = at(TokenKind::Minus) ? unary() : postfix();
    }
    ExprPtr step_value;
    const bool has_step = match(TokenKind::Colon);
    if (has_step) {
        if (missing_endpoint())
            throw CompileError(Diagnostic{"SLICE_STEP", "Range step is missing.", peek().span});
        FlagScope no_nested_range(slice_start_marker_stop_, true);
        step_value = unary();
    }
    if (at(TokenKind::Colon))
        throw CompileError(Diagnostic{"SLICE_STEP", "Range takes at most one step.", peek().span});
    auto synthetic_int = [&](std::uint64_t value) {
        auto e = std::make_unique<Expr>();
        e->span = colon.span;
        e->data = IntegerExpr{value, std::to_string(value), true};
        return e;
    };
    auto synthetic_bool = [&](bool value) {
        auto e = std::make_unique<Expr>();
        e->span = colon.span;
        e->data = BoolExpr{value};
        return e;
    };
    if (!begin_value) begin_value = synthetic_int(0);
    if (!step_value) step_value = synthetic_int(0);
    const auto end = has_step ? step_value->span.end : end_value->span.end;
    std::vector<CallArg> args;
    auto arg = [&](ExprPtr value) {
        auto span = value->span;
        args.push_back(CallArg{std::nullopt, false, std::move(value), span});
    };
    arg(std::move(begin_value));
    arg(std::move(end_value));
    arg(std::move(step_value));
    arg(synthetic_bool(start_exclusive));
    arg(synthetic_bool(end_exclusive));
    arg(synthetic_bool(has_step));
    arg(synthetic_bool(ascending_marker));
    arg(synthetic_bool(descending_marker));
    auto e = std::make_unique<Expr>();
    e->span = SourceSpan{begin, end};
    e->data = CallExpr{"$std.range.generate", std::move(args), {}};
    return e;
}

ExprPtr Parser::postfix() {
    auto e = primary();
    for (;;) {
        if(collection_header_source_ && at_collection_suffix()) break;
        if (at(TokenKind::LBracket) && peek(1).kind == TokenKind::RBracket &&
            peek(2).kind == TokenKind::LParen) {
            auto* name = std::get_if<NameExpr>(&e->data);
            if (name && !name->this_qualifier) {
                const auto start = e->span.start;
                consume(TokenKind::LBracket, "Expected '['.");
                consume(TokenKind::RBracket, "Expected ']'.");
                auto args = call_arguments();
                const auto end = previous().span.end;
                auto call = std::make_unique<Expr>();
                call->span = SourceSpan{start, end};
                call->data = CallExpr{name->name + "[]", std::move(args), {}};
                e = std::move(call);
                continue;
            }
        }

        if (looks_like_type_argument_call()) {
            auto* name = std::get_if<NameExpr>(&e->data);
            if (!name) {
                error(peek(), "Generic arguments require a named function or class.");
            }
            const auto start = e->span.start;
            auto type_arguments = type_argument_list();
            auto args = call_arguments();
            const auto end = previous().span.end;
            auto call = std::make_unique<Expr>();
            call->span = SourceSpan{start, end};
            call->data = CallExpr{name->name, std::move(args), std::move(type_arguments)};
            e = std::move(call);
            continue;
        }

        if (at(TokenKind::LParen)) {
            auto* name = std::get_if<NameExpr>(&e->data);
            if (!name) {
                error(peek(), "Only named functions and classes can be called directly.");
            }
            const auto start = e->span.start;
            auto args = call_arguments(name->name == "print" || name->name == "write");
            const auto end = previous().span.end;
            auto call = std::make_unique<Expr>();
            call->span = SourceSpan{start, end};
            call->data = CallExpr{name->name, std::move(args), {}};
            e = std::move(call);
            continue;
        }

        if (match(TokenKind::Dot)) {
            const auto start = e->span.start;
            if (at(TokenKind::Integer)) throw CompileError(Diagnostic{"TUPLE", "Tuples have no positional access; destructure the tuple instead.", peek().span});
            const auto member = consume(TokenKind::Identifier, "Expected member name after '.'.");

            std::vector<TypeName> type_arguments;
            if (looks_like_type_argument_call()) {
                type_arguments = type_argument_list();
            }

            if (at(TokenKind::LParen)) {
                auto args = call_arguments();
                const auto end = previous().span.end;
                auto call = std::make_unique<Expr>();
                call->span = SourceSpan{start, end};
                call->data = MethodCallExpr{std::move(e), member.text, std::move(args), std::move(type_arguments)};
                e = std::move(call);
            } else {
                if (!type_arguments.empty()) error(peek(), "Generic member arguments must be followed by a call.");
                auto access = std::make_unique<Expr>();
                access->span = SourceSpan{start, member.span.end};
                access->data = MemberExpr{std::move(e), member.text};
                e = std::move(access);
            }
            continue;
        }

        if (match(TokenKind::LBracket)) {
            const auto start = e->span.start;
            if (at(TokenKind::RBracket))
                throw CompileError(Diagnostic{"ELEMENT_ITERATION","'[]' element iteration is only valid in a for header.",peek().span});
            auto cursor=current_;
            if(scan_type_lookahead(tokens_,cursor,0,max_parse_depth) &&
               cursor+1<tokens_.size() && tokens_[cursor].kind==TokenKind::Identifier &&
               tokens_[cursor+1].kind==TokenKind::RBracket)
                throw CompileError(Diagnostic{"ELEMENT_ITERATION","'[nat i]' binds an index only in a for header.",peek().span});
            std::vector<IndexPart> items;
            for (;;) {
                items.push_back(index_item());
                if (!match(TokenKind::Comma)) break;
                if (at(TokenKind::RBracket)) {
                    error(peek(), "Trailing comma is not allowed in an index list.");
                }
            }
            const auto end = consume(TokenKind::RBracket, "Expected ']' after index.").span.end;
            auto indexed = std::make_unique<Expr>();
            indexed->span = SourceSpan{start, end};
            indexed->data = IndexExpr{std::move(e), std::move(items)};
            e = std::move(indexed);
            continue;
        }
        if (!slice_start_marker_stop_ &&
            (at(TokenKind::Colon) || at_marker_touching_colon())) {
            const auto begin = e->span.start;
            e = range_from(std::move(e), begin);
            continue;
        }
        break;
    }

    return e;
}

// One item of an index list: an index, or a slice
// `[start][marker]:[marker][end][:step]`. A marker is a '<' or '>'
// that touches the first colon; it makes its side exclusive and gives the
// direction ('<' ascending, '>' descending).
IndexPart Parser::index_item() {
    IndexPart item;
    const auto item_start = peek().span.start;
    if (at_marker_touching_colon()) {
        error(peek(), "An exclusive start needs a start value: write 's<:' or 's>:'.");
    }
    // After the first colon: an end marker touches it, and a marker on the
    // second colon is an error.
    const auto slice_tail = [&]() {
        item.slice = true;
        const auto& colon = previous();
        if (at(TokenKind::Less) || at(TokenKind::Greater)) {
            if (colon.span.end.offset != peek().span.start.offset)
                error(peek(), slice_marker_spacing_message);
            item.end_form = at(TokenKind::Less) ? SliceEnd::exclusive_ascending
                                                : SliceEnd::exclusive_descending;
            ++current_;
            if (at(TokenKind::Colon) || at(TokenKind::Comma) || at(TokenKind::RBracket) ||
                at(TokenKind::Eof) || at(TokenKind::Newline)) {
                error(peek(), "An exclusive end needs an end value: write ':<e' or ':>e'.");
            }
        }
        if (!at(TokenKind::Colon) && !at(TokenKind::Comma) && !at(TokenKind::RBracket)) {
            FlagScope marker_stop(slice_start_marker_stop_, true);
            item.stop = expression();
            if (item.end_form == SliceEnd::omitted) item.end_form = SliceEnd::inclusive;
            if (at_marker_touching_colon())
                error(peek(), "Slice markers belong to the first colon.");
        }
        if (match(TokenKind::Colon)) {
            if ((at(TokenKind::Less) || at(TokenKind::Greater)) &&
                previous().span.end.offset == peek().span.start.offset) {
                error(peek(), "Slice markers belong to the first colon.");
            }
            if (!at(TokenKind::Comma) && !at(TokenKind::RBracket)) {
                item.step = expression();
            }
        }
    };
    if (match(TokenKind::Colon)) {
        slice_tail();
    } else {
        ExprPtr first;
        {
            FlagScope marker_stop(slice_start_marker_stop_, true);
            first = expression();
        }
        // comparison() left a marker that touches the colon unconsumed.
        std::optional<SliceStart> marked;
        if (at_marker_touching_colon()) {
            marked = at(TokenKind::Less) ? SliceStart::exclusive_ascending
                                         : SliceStart::exclusive_descending;
            ++current_;
        }
        if (match(TokenKind::Colon)) {
            item.start = std::move(first);
            item.start_form = marked.value_or(SliceStart::inclusive);
            slice_tail();
        } else {
            item.index = std::move(first);
        }
    }
    item.span = SourceSpan{item_start, previous().span.end};
    return item;
}

// `this.NAME`: a field of the receiver. `this` is only a field qualifier,
// so every other use of it, and a call through it, is rejected here. The
// expression's span is the field name's, so every location recorded for the
// field is the same as for a bare name.
ExprPtr Parser::this_field() {
    const auto qualifier = consume(TokenKind::KwThis, "Expected this.");
    if (!at(TokenKind::Dot) || peek(1).kind != TokenKind::Identifier) {
        throw CompileError(Diagnostic{
            "THIS_QUALIFIER", "'this' only qualifies a field: write 'this.<field>'.", qualifier.span});
    }
    const auto dot = consume(TokenKind::Dot, "Expected '.' after this.");
    const auto name = consume(TokenKind::Identifier, "Expected a field name after 'this.'.");
    if (at(TokenKind::LParen) || looks_like_type_argument_call()) {
        throw CompileError(Diagnostic{
            "THIS_QUALIFIER",
            "Methods are called without 'this.': write '" + name.text +
                "(...)'; a function value held in a field is copied into a local first.",
            SourceSpan{qualifier.span.start, name.span.end}});
    }
    auto e = std::make_unique<Expr>();
    e->span = name.span;
    e->data = NameExpr{name.text, SourceSpan{qualifier.span.start, dot.span.end}};
    return e;
}

ExprPtr Parser::array_comprehension(SourcePos start) {
    const auto fail = [&](const std::string& message) {
        throw CompileError(Diagnostic{"ARRAY_COMPREHENSION", message, peek().span});
    };
    consume(TokenKind::KwFor, "Expected for.");
    auto cursor = current_;
    if (!scan_type_lookahead(tokens_, cursor, 0, max_parse_depth) ||
        cursor + 1 >= tokens_.size() || tokens_[cursor].kind != TokenKind::Identifier ||
        tokens_[cursor + 1].kind != TokenKind::Assign)
        fail("A generation expression declares one value binding: '[for auto x = values[] then x]'.");
    ArrayComprehensionExpr node;
    auto type = type_name();
    const auto name = consume(TokenKind::Identifier, "Expected generation binding name.");
    node.name = name.text;
    node.binding_span = {type.span.start, name.span.end};
    if (type.name != "auto" || !type.dimensions.empty()) node.type = std::move(type);
    if (node.name == "_") fail("A generation expression binds an element name.");
    consume(TokenKind::Assign, "Expected '=' before the generation source.");
    if (at(TokenKind::Ampersand)) fail("A generation expression takes values, not a reference source.");
    {
        FlagScope header(collection_header_source_, true);
        node.source = postfix();
    }
    if (!match(TokenKind::LBracket) || !match(TokenKind::RBracket))
        fail("A generation expression has one Array source ending in '[]'.");
    if (at(TokenKind::LBracket) || at(TokenKind::Comma) || at(TokenKind::KwFor))
        fail("A generation expression has exactly one source.");
    if (match(TokenKind::KwIf)) node.condition = expression();
    if (!match(TokenKind::KwThen)) fail("A generation expression requires 'then' and one value expression.");
    node.value = expression();
    if (!at(TokenKind::RBracket)) fail("A generation expression ends after its one value expression.");
    const auto end = consume(TokenKind::RBracket, "Expected ']' after generation expression.").span.end;
    auto result = std::make_unique<Expr>();
    result->span = {start, end};
    result->data = std::move(node);
    return result;
}

ExprPtr Parser::primary() {
    if (!slice_start_marker_stop_ && at(TokenKind::Colon))
        return range_from(nullptr, peek().span.start);
    if (match(TokenKind::Integer)) {
        const auto t=previous(); std::uint64_t value{}; const auto* b=t.text.data(); const auto* end=b+t.text.size();
        const auto parsed=std::from_chars(b,end,value);
        const bool fits=parsed.ec!=std::errc::result_out_of_range&&parsed.ptr==end;
        if(parsed.ec!=std::errc{}&&parsed.ec!=std::errc::result_out_of_range)
            error(t,"Invalid integer literal.");
        auto e=std::make_unique<Expr>(); e->span=t.span;
        e->data=IntegerExpr{fits?value:0,t.text,fits}; return e;
    }
    if (match(TokenKind::RealLiteral)) {
        const auto t=previous(); char* end=nullptr; const auto value=std::strtod(t.text.c_str(),&end);
        if (!end || *end!='\0') error(t,"Invalid real literal.");
        auto e=std::make_unique<Expr>(); e->span=t.span; e->data=RealLiteralExpr{value,t.text}; return e;
    }
    if (match(TokenKind::ImaginaryLiteral)) {
        const auto t=previous(); auto magnitude=t.text.substr(0,t.text.size()-1);
        const bool integer_form=magnitude.find('.')==std::string::npos;
        auto e=std::make_unique<Expr>(); e->span=t.span;
        e->data=ImaginaryLiteralExpr{std::move(magnitude),integer_form}; return e;
    }
    if (match(TokenKind::String)) { const auto t=previous(); return string_expression(t); }
    if (match(TokenKind::KwTrue)||match(TokenKind::KwFalse)) { const auto t=previous(); auto e=std::make_unique<Expr>(); e->span=t.span; e->data=BoolExpr{t.kind==TokenKind::KwTrue}; return e; }
    if (match(TokenKind::LBracket)) {
        FlagScope header_stop(collection_header_source_, false);
    FlagScope marker_stop(slice_start_marker_stop_, false);
        const auto start=previous().span.start;
        if (at(TokenKind::KwFor)) return array_comprehension(start);
        std::vector<ExprPtr> elements;
        if (!at(TokenKind::RBracket)) { do { elements.push_back(expression()); } while(match(TokenKind::Comma) && !at(TokenKind::RBracket)); }
        const auto end=consume(TokenKind::RBracket,"Expected ']' after array literal.").span.end;
        auto e=std::make_unique<Expr>(); e->span=SourceSpan{start,end}; e->data=ArrayExpr{std::move(elements)}; return e;
    }
    if (match(TokenKind::LParen)) { FlagScope header_stop(collection_header_source_, false);
    FlagScope marker_stop(slice_start_marker_stop_, false); const auto start=previous().span.start;
        if(at(TokenKind::RParen)) throw CompileError(Diagnostic{"TUPLE","A tuple has at least two elements.",peek().span});
        auto e=expression();
        if(match(TokenKind::Comma)) {
            std::vector<ExprPtr> elements;elements.push_back(std::move(e));
            do {
                if(at(TokenKind::RParen)) throw CompileError(Diagnostic{"TUPLE","Trailing comma is not allowed in a tuple.",peek().span});
                elements.push_back(expression());
            } while(match(TokenKind::Comma));
            e=std::make_unique<Expr>();e->data=TupleExpr{std::move(elements)};
        }
        const auto end=consume(TokenKind::RParen,"Expected ')' after expression.").span.end;
        e->span=SourceSpan{start,end};return e;
    }
    if (at(TokenKind::KwThis)) return this_field();
    if (at(TokenKind::LBrace) || at(TokenKind::RBrace)) error(peek(), braces_outside_shape_message);
    if (at(TokenKind::KwIf)) {
        report_if_expression(
            peek(),
            "An if-expression used as an operand must be parenthesized: write '(if ... then ... else ...)'.");
        return if_expression();
    }
    if (match(TokenKind::Identifier)) {
        const auto t=previous(); auto e=std::make_unique<Expr>(); e->span=t.span;
        if (t.text=="void") e->data=VoidExpr{};
        else if (t.text=="none") e->data=NoneExpr{};
        else e->data=NameExpr{t.text};
        return e;
    }
    error(peek(),"Expected expression.");
}

} // namespace quidra
