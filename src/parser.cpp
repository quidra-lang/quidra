#include "quidra/parser.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string_view>
#include <system_error>

namespace quidra {
namespace {
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

bool scan_type_lookahead(const std::vector<Token>& tokens, std::size_t& index,
                         std::size_t depth, std::size_t limit) {
    if (tokens.empty()) return false;
    if (depth > limit) {
        const auto& token = tokens[std::min(index, tokens.size() - 1)];
        throw CompileError(Diagnostic{
            "PARSE_DEPTH",
            "Type nesting exceeds the parser limit.",
            token.span});
    }
    if (index >= tokens.size() || tokens[index].kind != TokenKind::Identifier) return false;
    const auto root_name = tokens[index++].text;
    bool qualified = false;
    while (index < tokens.size() && tokens[index].kind == TokenKind::Dot) {
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
                if (kind == TokenKind::Integer || kind == TokenKind::Identifier) {
                    ++index;
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

    if (index < tokens.size() && tokens[index].kind == TokenKind::Less) {
        if (!qualified && root_name == "tensor") {
            ++index;
            if (!scan_type_lookahead(tokens, index, depth + 1, limit)) return false;
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::Greater) return false;
            if (index < tokens.size() && tokens[index].kind == TokenKind::Less &&
                !scan_shape()) return false;
        } else if (!qualified && root_name == "neural" && index + 1 < tokens.size() &&
                   (tokens[index + 1].kind == TokenKind::Integer ||
                    tokens[index + 1].kind == TokenKind::Plus ||
                    tokens[index + 1].kind == TokenKind::Minus ||
                    tokens[index + 1].kind == TokenKind::LParen ||
                    (tokens[index + 1].kind == TokenKind::Identifier &&
                     (tokens[index + 1].text == "_" ||
                      (tokens[index + 1].text != "float" &&
                       tokens[index + 1].text != "float32" &&
                       (!tokens[index + 1].text.empty() &&
                        std::islower(static_cast<unsigned char>(
                            tokens[index + 1].text.front())))))))) {
            if (!scan_shape()) return false;
        } else {
            ++index;
            if (!scan_type_lookahead(tokens, index, depth + 1, limit)) return false;
            while (index < tokens.size() && tokens[index].kind == TokenKind::Comma) {
                ++index;
                if (!scan_type_lookahead(tokens, index, depth + 1, limit)) return false;
            }
            if (index >= tokens.size() || tokens[index++].kind != TokenKind::Greater) return false;
            if (!qualified && root_name == "neural" &&
                index < tokens.size() && tokens[index].kind == TokenKind::Less &&
                !scan_shape()) return false;
        }
    }
    while (index < tokens.size() && tokens[index].kind == TokenKind::LBracket) {
        ++index;
        if (index < tokens.size() && tokens[index].kind != TokenKind::RBracket) {
            if (!scan_integer_expr(TokenKind::RBracket, TokenKind::RBracket)) return false;
        }
        if (index >= tokens.size() || tokens[index++].kind != TokenKind::RBracket) return false;
    }
    if (index < tokens.size() && tokens[index].kind == TokenKind::Pipe) {
        ++index;
        return scan_type_lookahead(tokens, index, depth + 1, limit);
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
}

void relocate_arg(CallArg& argument, const SourcePos& base) {
    relocate_span(argument.span, base);
    relocate_expr(*argument.value, base);
}

void relocate_expr(Expr& expression, const SourcePos& base) {
    relocate_span(expression.span, base);
    if (auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (auto& child : node->expressions) relocate_expr(*child, base);
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
        relocate_expr(*node->left, base);
        relocate_expr(*node->right, base);
    } else if (auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (auto& argument : node->args) relocate_arg(argument, base);
        for (auto& argument : node->type_arguments) relocate_type(argument, base);
    } else if (auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        relocate_expr(*node->receiver, base);
        for (auto& argument : node->args) relocate_arg(argument, base);
        for (auto& argument : node->type_arguments) relocate_type(argument, base);
    } else if (auto* node = std::get_if<TryExpr>(&expression.data)) {
        relocate_expr(*node->value, base);
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
    error(peek(), std::string("Expected newline after ") + context + ".");
}

bool Parser::looks_like_declaration(bool function) const {
    std::size_t i = current_;
    if (!function && i < tokens_.size() && tokens_[i].kind == TokenKind::KwConst) ++i;
    if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth)) return false;
    if (!function && i < tokens_.size() && tokens_[i].kind == TokenKind::Ampersand) ++i;
    if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;

    if (function && i < tokens_.size() && tokens_[i].kind == TokenKind::Less) {
        ++i;
        if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;
        while (i < tokens_.size() && tokens_[i].kind == TokenKind::Comma) {
            ++i;
            if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Identifier) return false;
        }
        if (i >= tokens_.size() || tokens_[i++].kind != TokenKind::Greater) return false;
    }

    if (i >= tokens_.size()) return false;
    return function ? tokens_[i].kind == TokenKind::LParen : tokens_[i].kind != TokenKind::LParen;
}

bool Parser::looks_like_type_argument_call() const {
    if (!at(TokenKind::Less)) return false;

    std::size_t i = current_ + 1;
    if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth)) return false;
    while (i < tokens_.size() && tokens_[i].kind == TokenKind::Comma) {
        ++i;
        if (!scan_type_lookahead(tokens_, i, 1, max_parse_depth)) return false;
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

Program Parser::parse() {
    Program p;
    p.language_version = std::string(language_version);
    consume_newlines();
    while (!at(TokenKind::Eof)) {
        try {
            if (at(TokenKind::KwImport)) p.imports.push_back(import_decl());
            else if (at(TokenKind::Identifier) && peek().text == "extern") p.functions.push_back(external_function_decl());
            else if (looks_like_cli_decl()) cli_decl(p);
            else if (at(TokenKind::KwClass)) p.classes.push_back(class_decl());
            else if (at(TokenKind::KwOverride)) error(peek(), "override is only valid on a class method.");
            else if (looks_like_declaration(true)) p.functions.push_back(function_decl());
            else p.statements.push_back(statement());
        } catch (const CompileError& error) {
            record(error);
            synchronize_statement();
        }
        consume_newlines();
    }
    if (!diagnostics_.empty()) throw CompileErrors(std::move(diagnostics_));
    return p;
}

TypeName Parser::type_name() {
    ParseDepthGuard depth(type_depth_, max_parse_depth, peek(), "Type");
    TypeName t;
    const auto first = consume(TokenKind::Identifier, "Expected an explicit type.");
    t.name = first.text;
    t.span = first.span;

    while (match(TokenKind::Dot)) {
        const auto part = consume(TokenKind::Identifier, "Expected type name after '.'.");
        t.name += "." + part.text;
        t.span.end = part.span.end;
    }

    const auto parse_shape_pattern = [&]() {
        consume(TokenKind::Less, "Expected '<' before shape pattern.");
        if (at(TokenKind::Greater)) error(peek(), "Shape pattern requires at least one axis.");
        while (true) {
            if (at(TokenKind::Identifier) && peek().text == "_" &&
                (peek(1).kind == TokenKind::Comma || peek(1).kind == TokenKind::Greater)) {
                consume(TokenKind::Identifier, "Expected '_'.");
                t.tensor_shape_prefix.push_back(-1);
                t.tensor_shape_expressions.push_back(nullptr);
            } else {
                auto parsed = type_integer_expression();
                long long static_extent = -2;
                if (const auto* literal = std::get_if<IntegerExpr>(&parsed->data)) {
                    if (literal->value >
                        static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
                        error(previous(), "Shape extent is too large.");
                    }
                    static_extent = static_cast<long long>(literal->value);
                }
                t.tensor_shape_prefix.push_back(static_extent);
                t.tensor_shape_expressions.emplace_back(parsed.release());
            }
            if (!match(TokenKind::Comma)) break;
            if (at(TokenKind::Greater)) error(peek(), "Trailing comma is not allowed in a shape pattern.");
        }
        consume(TokenKind::Greater, "Expected '>' after shape pattern.");
        t.tensor_rank = static_cast<long long>(t.tensor_shape_prefix.size());
        t.span.end = previous().span.end;
    };

    if (at(TokenKind::Less)) {
        if (t.name == "tensor") {
            consume(TokenKind::Less, "Expected '<' before tensor element type.");
            if (at(TokenKind::Greater)) error(peek(), "tensor requires an element type.");
            t.arguments.push_back(type_name());
            consume(TokenKind::Greater, "Expected '>' after tensor element type.");
            t.span.end = previous().span.end;
            if (at(TokenKind::Less)) parse_shape_pattern();
        } else if (t.name == "neural" &&
                   (peek(1).kind == TokenKind::Integer ||
                    peek(1).kind == TokenKind::Plus ||
                    peek(1).kind == TokenKind::Minus ||
                    peek(1).kind == TokenKind::LParen ||
                    (peek(1).kind == TokenKind::Identifier &&
                     (peek(1).text == "_" ||
                      (peek(1).text != "float" &&
                       peek(1).text != "float32" &&
                       !peek(1).text.empty() &&
                       std::islower(static_cast<unsigned char>(
                           peek(1).text.front()))))))) {
            parse_shape_pattern();
        } else {
            t.arguments = type_argument_list();
            t.span.end = previous().span.end;
            if (t.name == "neural" && at(TokenKind::Less)) parse_shape_pattern();
        }
    }
    while (match(TokenKind::LBracket)) {
        long long length = -1;
        std::shared_ptr<Expr> dimension;
        if (!at(TokenKind::RBracket)) {
            auto parsed = type_integer_expression();
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
        result->data = IntegerExpr{value};
        return result;
    }

    if (match(TokenKind::Identifier)) {
        const auto token = previous();
        if (token.text == "_") {
            error(token, "'_' is only valid as a tensor/neural wildcard axis.");
        }
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
    } while (match(TokenKind::Comma));
    consume(TokenKind::Greater, "Expected '>' after type arguments.");
    return arguments;
}

std::vector<std::string> Parser::type_parameter_list() {
    if (!match(TokenKind::Less)) return {};
    std::vector<std::string> parameters;
    if (at(TokenKind::Greater)) error(peek(), "Generic type parameter list cannot be empty.");
    do {
        parameters.push_back(consume(TokenKind::Identifier, "Expected generic type parameter name.").text);
    } while (match(TokenKind::Comma));
    consume(TokenKind::Greater, "Expected '>' after generic type parameters.");
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
        out->data = IntegerExpr{static_cast<std::uint64_t>(value)};
        return out;
    };

    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        const auto field_start = peek().span.start;
        auto type = type_name();
        const auto field = consume(TokenKind::Identifier, "Expected CLI field name.");
        consume(TokenKind::Assign, "CLI fields require '= argument()', '= option(default = ...)', or '= flag()'.");
        auto declaration = expression();
        auto* call = std::get_if<CallExpr>(&declaration->data);
        if (!call || (call->callee != "argument" && call->callee != "option" && call->callee != "flag")) {
            error(field, "CLI fields must use argument(), option(default = ...), or flag().");
        }

        auto hidden = std::make_unique<Expr>();
        hidden->span = declaration->span;
        std::vector<CallArg> hidden_args;
        hidden_args.push_back(CallArg{std::nullopt, false, string_expr(field.text, field.span), field.span});

        if (call->callee == "argument") {
            if (!call->args.empty()) error(field, "argument() takes no arguments.");
            hidden_args.push_back(CallArg{std::nullopt, false, integer_expr(positional_index++, field.span), field.span});
            hidden->data = CallExpr{"$std.cli.argument", std::move(hidden_args), {}};
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
    binding_stmt->data = BindingStmt{std::move(cli_type), binding.text, std::move(constructor), false, false};
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

ClassDecl Parser::class_decl() {
    const auto start = consume(TokenKind::KwClass, "Expected class.").span.start;
    const auto name = consume(TokenKind::Identifier, "Expected class name.");
    auto type_parameters = type_parameter_list();

    std::optional<std::string> parent;
    std::optional<TypeName> parent_type;
    if (match(TokenKind::Colon)) {
        parent_type = type_name();
        parent = parent_type->name;
    }

    end_statement("class signature");
    consume(TokenKind::Indent, "Expected class body indented by four spaces.");

    std::vector<FieldDecl> fields;
    std::vector<FunctionDecl> methods;
    consume_newlines();
    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        if (at(TokenKind::KwClass)) error(peek(), "Nested classes are prohibited.");
        if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
        if (at(TokenKind::KwOverride)) {
            methods.push_back(function_decl(true));
        } else if (looks_like_declaration(true)) {
            methods.push_back(function_decl(true));
        } else if (looks_like_declaration(false)) {
            const auto field_start = peek().span.start;
            const bool is_const = match(TokenKind::KwConst);
            auto type = type_name();
            const auto field = consume(TokenKind::Identifier, "Expected field name.");
            ExprPtr default_value;
            if (match(TokenKind::Assign)) default_value = expression();
            const auto field_end = default_value ? default_value->span.end : field.span.end;
            auto span = SourceSpan{field_start, field_end};
            end_statement("class field");
            fields.push_back(FieldDecl{field.text, std::move(type), span, std::move(default_value), is_const});
        } else {
            error(peek(), "Expected a field or method declaration in class body.");
        }
        consume_newlines();
    }
    const auto end = consume(TokenKind::Dedent, "Expected end of class body.").span.end;
    return ClassDecl{name.text, std::move(parent), std::move(fields), std::move(methods),
                     {start, end}, std::move(type_parameters), std::move(parent_type)};
}

FunctionDecl Parser::function_decl(bool allow_override) {
    const auto declaration_start = peek().span.start;
    const bool is_override=match(TokenKind::KwOverride);
    if(is_override && !allow_override) error(previous(),"override is only valid on a class method.");
    auto result=type_name();auto start=declaration_start;
    auto name=consume(TokenKind::Identifier,"Expected function name.");
    auto type_parameters=type_parameter_list();
    consume(TokenKind::LParen,"Expected '('."); std::vector<Parameter> params;
    while(!at(TokenKind::RParen)) {
        const bool is_const=match(TokenKind::KwConst);
        auto type=type_name();bool write=match(TokenKind::Ampersand);auto p=consume(TokenKind::Identifier,"Expected parameter name.");
        ExprPtr value; if(match(TokenKind::Assign)) value=expression();
        params.push_back(Parameter{p.text,std::move(type),write,p.span,std::move(value),is_const});
        if(!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RParen,"Expected ')'.");end_statement("function signature");auto body=block_until(false);
    auto end=previous().span.end;
    return FunctionDecl{name.text, std::move(params), std::move(result), std::move(body),
                        {start, end}, is_override, std::move(type_parameters), std::nullopt};
}


FunctionDecl Parser::external_function_decl() {
    const auto start=consume(TokenKind::Identifier,"Expected extern.").span.start;
    auto result=type_name();
    const auto name=consume(TokenKind::Identifier,"Expected external function name.");
    if(at(TokenKind::Less))
        error(peek(),"External C functions cannot be generic.");
    consume(TokenKind::LParen,"Expected '(' after external function name.");
    std::vector<Parameter> parameters;
    while(!at(TokenKind::RParen)) {
        const bool is_const=match(TokenKind::KwConst);
        auto type=type_name();
        const bool reference=match(TokenKind::Ampersand);
        const auto parameter=consume(TokenKind::Identifier,"Expected external parameter name.");
        ExprPtr default_value;
        if(match(TokenKind::Assign)) default_value=expression();
        parameters.push_back(Parameter{
            parameter.text,std::move(type),reference,parameter.span,
            std::move(default_value),is_const});
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
    declaration.external_symbol=symbol.text;
    return declaration;
}

std::vector<StmtPtr> Parser::block_until(bool) {
    consume(TokenKind::Indent, "Expected a block indented by four spaces.");
    std::vector<StmtPtr> body;
    while (!at(TokenKind::Dedent) && !at(TokenKind::Eof)) {
        try {
            if (at(TokenKind::KwClass)) error(peek(), "Nested classes are prohibited.");
            if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
            if (at(TokenKind::Identifier) && peek().text == "extern") error(peek(), "extern declarations are only valid at top level.");
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
    if (at(TokenKind::KwReturn)) return return_stmt();
    if (at(TokenKind::KwBreak) || at(TokenKind::KwContinue)) return loop_control_stmt();
    if (at(TokenKind::KwIf)) return if_stmt();
    if (at(TokenKind::KwWhile)) return while_stmt();
    if (at(TokenKind::KwFor)) return for_stmt();
    if (at(TokenKind::KwMatch)) return match_stmt();
    if (at(TokenKind::KwImport)) error(peek(), "import is only valid at top level.");
    if (at(TokenKind::KwClass) || at(TokenKind::KwOverride)) error(peek(),"class and override are only valid at class declaration boundaries.");
    if (looks_like_declaration(false)) return binding_stmt();
    if (at(TokenKind::Ampersand)) return rebind_stmt();
    return expr_or_assign_stmt();
}

StmtPtr Parser::binding_stmt() {
    const auto start=peek().span.start;
    const bool is_const=match(TokenKind::KwConst);
    auto type=type_name();
    const bool reference=match(TokenKind::Ampersand);
    auto name=consume(TokenKind::Identifier,"Expected binding name.");
    ExprPtr value; bool reference_initializer=false;
    if(match(TokenKind::Assign)) {
        reference_initializer=match(TokenKind::Ampersand);
        value=expression();
    }
    auto end=value?value->span.end:name.span.end;
    end_statement("binding");
    auto s=std::make_unique<Stmt>();s->span={start,end};
    s->data=BindingStmt{std::move(type),name.text,std::move(value),reference,reference_initializer,is_const};
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
        auto condition = expression();
        end_statement("condition");
        auto then_body = block_until(false);
        std::vector<StmtPtr> else_body;
        if (at(TokenKind::KwElif)) {
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
StmtPtr Parser::while_stmt() {
    auto start=consume(TokenKind::KwWhile,"Expected while.").span.start;auto cond=expression();end_statement("condition");auto body=block_until(false);auto s=std::make_unique<Stmt>();s->span={start,previous().span.end};s->data=WhileStmt{std::move(cond),std::move(body)};return s;
}
StmtPtr Parser::for_stmt() {
    auto start=consume(TokenKind::KwFor,"Expected for.").span.start;bool write=match(TokenKind::Ampersand);auto name=consume(TokenKind::Identifier,"Expected iteration name.");consume(TokenKind::KwIn,"Expected in.");auto value=expression();end_statement("iterable");auto body=block_until(false);auto s=std::make_unique<Stmt>();s->span={start,previous().span.end};s->data=ForStmt{name.text,write,std::move(value),std::move(body)};return s;
}
StmtPtr Parser::match_stmt() {
    auto start=consume(TokenKind::KwMatch,"Expected match.").span.start;auto value=expression();end_statement("match value");consume(TokenKind::Indent,"Expected indented typed cases.");std::vector<MatchCase> cases;
    while(!at(TokenKind::Dedent)&&!at(TokenKind::Eof)) {auto type=type_name();auto span=type.span;auto tag=type.name;std::optional<std::string> binder;if(at(TokenKind::Identifier)){auto n=consume(TokenKind::Identifier,"Expected binder.");binder=n.text;span.end=n.span.end;}end_statement("match case");auto body=block_until(false);cases.push_back(MatchCase{std::move(type),tag,std::move(binder),std::move(body),span});}
    consume(TokenKind::Dedent,"Expected end of match.");auto s=std::make_unique<Stmt>();s->span={start,previous().span.end};s->data=MatchStmt{std::move(value),std::move(cases)};return s;
}

StmtPtr Parser::rebind_stmt() {
    const auto start=consume(TokenKind::Ampersand,"Expected '&'.").span.start;
    const auto name=consume(TokenKind::Identifier,"Expected reference binding name after '&'.");
    consume(TokenKind::Assign,"Expected '=' in address assignment.");
    consume(TokenKind::Ampersand,"Address assignment requires '&' on the right-hand side.");
    auto target=expression();
    const auto end=target->span.end;
    end_statement("address assignment");
    auto stmt=std::make_unique<Stmt>();stmt->span={start,end};
    stmt->data=RebindStmt{name.text,std::move(target)};
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
        stmt->data = AssignStmt{std::move(left), std::move(value), std::move(compound_op)}; return stmt;
    }
    const auto span = left->span;
    end_statement("expression");
    auto stmt = std::make_unique<Stmt>(); stmt->span = span; stmt->data = ExprStmt{std::move(left)}; return stmt;
}

std::vector<CallArg> Parser::call_arguments() {
    consume(TokenKind::LParen,"Expected '('.");
    std::vector<CallArg> args;
    if (!at(TokenKind::RParen)) {
        do {
            std::optional<std::string> label;
            bool writable = false;
            SourceSpan arg_span = peek().span;
            if (at(TokenKind::Ampersand) && peek(1).kind == TokenKind::Identifier &&
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
                    if (at(TokenKind::Ampersand)) {
                        error(peek(), "Named writable arguments use '&name = &value', not 'name = &value'.");
                    }
                }
                writable = match(TokenKind::Ampersand);
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
    return or_expr();
}

ExprPtr Parser::inline_expression() {
    consume_newlines();
    auto result = expression();
    consume_newlines();
    if (!at(TokenKind::Eof)) error(peek(), "Unexpected token inside string interpolation.");
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
            std::size_t j = expression_start; int paren = 0, bracket = 0; bool in_string = false;
            for (; j < token.text.size(); ++j) {
                const char c = token.text[j];
                if (in_string) { if (c=='"') in_string=false; continue; }
                if (c=='"') { in_string=true; continue; }
                if (c=='(') ++paren; else if (c==')') --paren; else if (c=='[') ++bracket; else if (c==']') --bracket;
                else if (c=='}' && paren==0 && bracket==0) break;
            }
            if (j >= token.text.size()) error(token, "Unclosed '{' in string interpolation.");

            const auto inner = std::string_view(token.text).substr(expression_start, j-expression_start);
            std::size_t colon = std::string_view::npos;
            paren = 0; bracket = 0; in_string = false;
            for (std::size_t k = 0; k < inner.size(); ++k) {
                const char c = inner[k];
                if (in_string) { if (c=='"') in_string=false; continue; }
                if (c=='"') { in_string=true; continue; }
                if (c=='(') ++paren; else if (c==')') --paren; else if (c=='[') ++bracket; else if (c==']') --bracket;
                else if (c==':' && paren==0 && bracket==0) { colon = k; break; }
            }
            const auto expression_text = colon == std::string_view::npos ? inner : inner.substr(0, colon);
            if (trim(expression_text).empty()) error(token, "String interpolation cannot be empty.");
            const auto base = string_content_position(token, expression_start);
            try {
                Parser nested(Lexer(std::string(expression_text)).scan(), max_errors_);
                auto nested_expression = nested.inline_expression();
                relocate_expr(*nested_expression, base);
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
    expr->data = BinaryExpr{op.text, std::move(left), std::move(right)}; return expr;
}
ExprPtr Parser::or_expr() { auto e=and_expr(); while(match(TokenKind::KwOr)){auto op=previous(); e=make_binary(std::move(e),op,and_expr());} return e; }
ExprPtr Parser::and_expr() { auto e=equality(); while(match(TokenKind::KwAnd)){auto op=previous(); e=make_binary(std::move(e),op,equality());} return e; }
ExprPtr Parser::equality() { auto e=comparison(); while(match(TokenKind::EqEq)||match(TokenKind::NotEq)){auto op=previous(); e=make_binary(std::move(e),op,comparison());} return e; }
ExprPtr Parser::comparison() { auto e=term(); while(match(TokenKind::Less)||match(TokenKind::LessEq)||match(TokenKind::Greater)||match(TokenKind::GreaterEq)){auto op=previous(); e=make_binary(std::move(e),op,term());} return e; }
ExprPtr Parser::term() { auto e=factor(); while(match(TokenKind::Plus)||match(TokenKind::Minus)){auto op=previous(); e=make_binary(std::move(e),op,factor());} return e; }
ExprPtr Parser::factor() { auto e=unary(); while(match(TokenKind::Star)||match(TokenKind::Slash)||match(TokenKind::Percent)){auto op=previous(); e=make_binary(std::move(e),op,unary());} return e; }

ExprPtr Parser::unary() {
    ParseDepthGuard depth(unary_depth_, max_parse_depth, peek(), "Unary expression");
    if (match(TokenKind::KwTry)) {
        const auto start = previous().span.start;
        auto value = unary();
        auto e=std::make_unique<Expr>(); e->span=SourceSpan{start,value->span.end}; e->data=TryExpr{std::move(value)}; return e;
    }
    if (match(TokenKind::KwNot) || match(TokenKind::Minus)) {
        const auto op=previous(); auto operand=unary(); auto e=std::make_unique<Expr>();
        e->span=SourceSpan{op.span.start,operand->span.end}; e->data=UnaryExpr{op.text,std::move(operand)}; return e;
    }
    return postfix();
}

ExprPtr Parser::postfix() {
    auto e = primary();
    for (;;) {
        if (looks_like_type_argument_call()) {
            auto* name = std::get_if<NameExpr>(&e->data);
            if (!name || name->name == "super") {
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
            if (!name || name->name == "super") {
                error(peek(), "Only named functions and classes can be called directly.");
            }
            const auto start = e->span.start;
            auto args = call_arguments();
            const auto end = previous().span.end;
            auto call = std::make_unique<Expr>();
            call->span = SourceSpan{start, end};
            call->data = CallExpr{name->name, std::move(args), {}};
            e = std::move(call);
            continue;
        }

        if (match(TokenKind::Dot)) {
            const auto start = e->span.start;
            const bool super_receiver =
                std::holds_alternative<NameExpr>(e->data) &&
                std::get<NameExpr>(e->data).name == "super";
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
                if (super_receiver) error(member, "super is only valid as super.method(...).");
                if (!type_arguments.empty()) error(peek(), "Generic member arguments must be followed by a call.");
                auto access = std::make_unique<Expr>();
                access->span = SourceSpan{start, member.span.end};
                access->data = MemberExpr{std::move(e), member.text};
                e = std::move(access);
            }
            continue;
        }

        if (match(TokenKind::LBracket)) {
            if (std::holds_alternative<NameExpr>(e->data) &&
                std::get<NameExpr>(e->data).name == "super") {
                error(previous(), "super cannot be indexed.");
            }
            const auto start = e->span.start;
            if (at(TokenKind::RBracket)) error(peek(), "Index list cannot be empty.");
            std::vector<IndexPart> items;
            for (;;) {
                IndexPart item;
                const auto item_start = peek().span.start;
                if (match(TokenKind::Colon)) {
                    item.slice = true;
                    if (!at(TokenKind::Colon) && !at(TokenKind::Comma) &&
                        !at(TokenKind::RBracket)) {
                        item.stop = expression();
                    }
                    if (match(TokenKind::Colon)) {
                        if (!at(TokenKind::Comma) && !at(TokenKind::RBracket)) {
                            item.step = expression();
                        }
                    }
                } else {
                    auto first = expression();
                    if (match(TokenKind::Colon)) {
                        item.slice = true;
                        item.start = std::move(first);
                        if (!at(TokenKind::Colon) && !at(TokenKind::Comma) &&
                            !at(TokenKind::RBracket)) {
                            item.stop = expression();
                        }
                        if (match(TokenKind::Colon)) {
                            if (!at(TokenKind::Comma) && !at(TokenKind::RBracket)) {
                                item.step = expression();
                            }
                        }
                    } else {
                        item.index = std::move(first);
                    }
                }
                item.span = SourceSpan{item_start, previous().span.end};
                items.push_back(std::move(item));
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
        break;
    }

    if (std::holds_alternative<NameExpr>(e->data) &&
        std::get<NameExpr>(e->data).name == "super") {
        error(previous(), "super is only valid as super.method(...).");
    }
    return e;
}

ExprPtr Parser::primary() {
    if (match(TokenKind::Integer)) {
        const auto t=previous(); std::uint64_t value{}; const auto* b=t.text.data(); const auto* end=b+t.text.size();
        const auto parsed=std::from_chars(b,end,value);
        if(parsed.ec==std::errc::result_out_of_range||parsed.ptr!=end) throw CompileError(Diagnostic{"INTEGER_RANGE","Integer literal is outside the uint64 range.",t.span});
        auto e=std::make_unique<Expr>(); e->span=t.span; e->data=IntegerExpr{value}; return e;
    }
    if (match(TokenKind::Float)) {
        const auto t=previous(); char* end=nullptr; const auto value=std::strtod(t.text.c_str(),&end);
        if (!end || *end!='\0') error(t,"Invalid float literal.");
        auto e=std::make_unique<Expr>(); e->span=t.span; e->data=FloatExpr{value}; return e;
    }
    if (match(TokenKind::String)) { const auto t=previous(); return string_expression(t); }
    if (match(TokenKind::KwTrue)||match(TokenKind::KwFalse)) { const auto t=previous(); auto e=std::make_unique<Expr>(); e->span=t.span; e->data=BoolExpr{t.kind==TokenKind::KwTrue}; return e; }
    if (match(TokenKind::LBracket)) {
        const auto start=previous().span.start; std::vector<ExprPtr> elements;
        if (!at(TokenKind::RBracket)) { do { elements.push_back(expression()); } while(match(TokenKind::Comma) && !at(TokenKind::RBracket)); }
        const auto end=consume(TokenKind::RBracket,"Expected ']' after array literal.").span.end;
        auto e=std::make_unique<Expr>(); e->span=SourceSpan{start,end}; e->data=ArrayExpr{std::move(elements)}; return e;
    }
    if (match(TokenKind::LParen)) { const auto start=previous().span.start; auto e=expression(); const auto end=consume(TokenKind::RParen,"Expected ')' after expression.").span.end; e->span=SourceSpan{start,end}; return e; }
    if (match(TokenKind::KwSuper)) {
        const auto t=previous(); auto e=std::make_unique<Expr>(); e->span=t.span; e->data=NameExpr{"super"}; return e;
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
