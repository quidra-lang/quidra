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
    std::size_t statement_depth_{};
    // Set while the start (or end) expression of an index item is parsed:
    // comparison() then stops before a '<' or '>' that touches the ':' after
    // it, which is a slice marker, not an operator. Cleared inside every
    // nested parenthesis, bracket, argument list and if-expression part
    // except the final else branch, so a marker belongs to the top level of
    // its own item.
    bool slice_start_marker_stop_{};
    bool collection_header_source_{};
    bool at_collection_suffix() const;
    std::vector<Diagnostic> diagnostics_;
    std::size_t max_errors_{20};
    // What the last comparison() call consumed at its own level: whether a
    // relational operator, and their tokens. equality() reads them to
    // flatten a relational comparison next to '==' or '!=' into one chain;
    // a parenthesized comparison is an operand and does not count.
    bool comparison_relational_{};
    std::vector<SourceSpan> comparison_op_spans_;
    // The offsets of the mutation markers accepted so far (Program).
    std::vector<std::size_t> mutation_markers_;
    // Consumes the '.' of a definition's mutation marker when one follows,
    // and records it.
    bool definition_marker();
    static constexpr std::size_t max_parse_depth = 128;
    // Statements get their own, looser budget: the checker bounds statement
    // nesting at 256 (src/nesting_budget.hpp), so a parser limit of 128 would
    // make that budget unreachable from source. This one only has to bound the
    // parser's own cycle, which costs ~608 bytes per level.
    static constexpr std::size_t max_parse_statement_depth = 512;

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
    bool looks_like_export_prefix() const;
    TypeName type_name();
    ExprPtr type_integer_expression();
    ExprPtr type_integer_term();
    ExprPtr type_integer_factor();
    std::vector<TypeName> type_argument_list();
    std::vector<std::string> type_parameter_list(std::vector<std::string>* constraints = nullptr);
    ImportDecl import_decl();
    void private_declaration(Program& program);
    void cli_decl(Program& program);
    EnumDecl enum_decl();
    ClassDecl class_decl();
    FunctionDecl function_decl();
    FunctionDecl external_function_decl();
    ForeignExport foreign_export_prefix();
    FunctionDecl export_function_decl();
    std::vector<StmtPtr> block_until(bool allow_else);
    StmtPtr statement();
    void reject_const_reference(bool is_const, bool reference) const;
    StmtPtr binding_stmt();
    bool pattern_followed_by_assignment() const;
    Pattern parse_pattern(bool match_case = false);
    StmtPtr destructuring_stmt();
    StmtPtr return_stmt();
    StmtPtr loop_control_stmt();
    StmtPtr if_stmt();
    StmtPtr main_guard_stmt();
    StmtPtr parallel_stmt();
    StmtPtr while_stmt();
    StmtPtr for_stmt();
    StmtPtr match_stmt();
    StmtPtr expr_or_assign_stmt();
    StmtPtr rebind_stmt();

    std::vector<CallArg> call_arguments(bool address_values = false);
    ExprPtr expression();
    ExprPtr if_expression();
    ExprPtr if_expression_part();
    void report_if_expression(const Token& token, std::string message);
    ExprPtr inline_expression();
    ExprPtr string_expression(const Token& token);
    ExprPtr this_field();
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
    ExprPtr power();
    ExprPtr marked_call();
    ExprPtr postfix();
    ExprPtr primary();
    ExprPtr range_from(ExprPtr begin_value, SourcePos begin);
    ExprPtr range_operand();
    ExprPtr array_comprehension(SourcePos start);
    ExprPtr make_binary(ExprPtr left, const Token& op, ExprPtr right);
    bool at_marker_touching_colon() const;
    IndexPart index_item();
};

} // namespace quidra
