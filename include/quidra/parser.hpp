#pragma once
#include "quidra/ast.hpp"
#include "quidra/token.hpp"
#include <vector>

namespace quidra {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::size_t max_errors = 20)
        : tokens_(std::move(tokens)), max_errors_(max_errors ? max_errors : 1) {}
    Program parse();
private:
    std::vector<Token> tokens_;
    std::size_t current_{};
    std::size_t expression_depth_{};
    std::size_t unary_depth_{};
    std::size_t type_depth_{};
    std::vector<Diagnostic> diagnostics_;
    std::size_t max_errors_{20};
    static constexpr std::size_t max_parse_depth = 128;

    const Token& peek(std::size_t n = 0) const;
    const Token& previous() const;
    bool at(TokenKind kind) const;
    bool match(TokenKind kind);
    const Token& consume(TokenKind kind, const char* message);
    void consume_newlines();
    void end_statement(const char* context);
    void record(const CompileError& error);
    void synchronize_statement();
    [[noreturn]] void error(const Token& token, std::string message) const;

    bool looks_like_declaration(bool function) const;
    bool looks_like_type_argument_call() const;
    bool looks_like_cli_decl() const;
    TypeName type_name();
    ExprPtr type_integer_expression();
    ExprPtr type_integer_term();
    ExprPtr type_integer_factor();
    std::vector<TypeName> type_argument_list();
    std::vector<std::string> type_parameter_list();
    ImportDecl import_decl();
    void cli_decl(Program& program);
    ClassDecl class_decl();
    FunctionDecl function_decl(bool allow_override = false);
    FunctionDecl external_function_decl();
    std::vector<StmtPtr> block_until(bool allow_else);
    StmtPtr statement();
    StmtPtr binding_stmt();
    StmtPtr return_stmt();
    StmtPtr loop_control_stmt();
    StmtPtr if_stmt();
    StmtPtr while_stmt();
    StmtPtr for_stmt();
    StmtPtr match_stmt();
    StmtPtr expr_or_assign_stmt();
    StmtPtr rebind_stmt();

    std::vector<CallArg> call_arguments();
    ExprPtr expression();
    ExprPtr inline_expression();
    ExprPtr string_expression(const Token& token);
    ExprPtr or_expr();
    ExprPtr and_expr();
    ExprPtr bit_or_expr();
    ExprPtr bit_xor_expr();
    ExprPtr bit_and_expr();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr shift_expr();
    ExprPtr term();
    ExprPtr factor();
    ExprPtr unary();
    ExprPtr postfix();
    ExprPtr primary();
    ExprPtr make_binary(ExprPtr left, const Token& op, ExprPtr right);
};

} // namespace quidra
