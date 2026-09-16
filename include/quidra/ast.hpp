#pragma once
#include "quidra/diagnostic.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace quidra {

struct TypeName {
    std::string name;
    std::vector<TypeName> arguments;
    std::size_t array_depth{};
    std::vector<long long> dimensions;
    SourceSpan span{};
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct IntegerExpr { std::uint64_t value{}; };
struct FloatExpr { double value{}; };
struct StringExpr { std::string value; };
struct InterpolationFormat {
    std::optional<std::uint32_t> integer_width;
    std::optional<std::uint32_t> fractional_digits;
    std::optional<std::uint32_t> significant_digits;
    bool zero{};
    bool active() const { return integer_width || fractional_digits || significant_digits || zero; }
};
struct StringTemplateExpr {
    std::vector<std::string> literals;
    std::vector<ExprPtr> expressions;
    std::vector<InterpolationFormat> formats;
};
struct BoolExpr { bool value{}; };
struct VoidExpr {};
struct NoneExpr {};
struct NameExpr { std::string name; };
struct ArrayExpr { std::vector<ExprPtr> elements; };
struct IndexPart {
    bool slice{};
    ExprPtr index;
    ExprPtr start;
    ExprPtr stop;
    ExprPtr step;
    SourceSpan span{};
};
struct IndexExpr { ExprPtr base; std::vector<IndexPart> items; };
struct MemberExpr { ExprPtr base; std::string name; };
struct UnaryExpr { std::string op; ExprPtr operand; };
struct BinaryExpr { std::string op; ExprPtr left; ExprPtr right; };
struct CallArg { std::optional<std::string> name; bool writable{}; ExprPtr value; SourceSpan span{}; };
struct CallExpr {
    std::string callee;
    std::vector<CallArg> args;
    std::vector<TypeName> type_arguments;
};
struct MethodCallExpr {
    ExprPtr receiver;
    std::string method;
    std::vector<CallArg> args;
    std::vector<TypeName> type_arguments;
};
struct TryExpr { ExprPtr value; };

struct Expr {
    using Data = std::variant<IntegerExpr, FloatExpr, StringExpr, StringTemplateExpr, BoolExpr, VoidExpr, NoneExpr,
                              NameExpr, ArrayExpr, IndexExpr, MemberExpr, UnaryExpr, BinaryExpr,
                              CallExpr, MethodCallExpr, TryExpr>;
    Data data;
    SourceSpan span{};
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct BindingStmt {
    TypeName declared_type;
    std::string name;
    ExprPtr value;
    bool reference{};
    bool reference_initializer{};
    bool is_const{};
};
struct AssignStmt { ExprPtr target; ExprPtr value; std::string compound_op; };
struct RebindStmt { std::string name; ExprPtr target; };
struct ReturnStmt { ExprPtr value; };
struct LoopControlStmt { bool is_continue{}; };
struct ExprStmt { ExprPtr value; };
struct IfStmt { ExprPtr condition; std::vector<StmtPtr> then_body; std::vector<StmtPtr> else_body; };
struct WhileStmt { ExprPtr condition; std::vector<StmtPtr> body; };
struct ForStmt { std::string name; bool writable{}; ExprPtr iterable; std::vector<StmtPtr> body; };
struct MatchCase { TypeName type; std::string tag; std::optional<std::string> binder; std::vector<StmtPtr> body; SourceSpan span{}; };
struct MatchStmt { ExprPtr value; std::vector<MatchCase> cases; };

struct Stmt {
    using Data = std::variant<BindingStmt, AssignStmt, RebindStmt, ReturnStmt, LoopControlStmt,
                              ExprStmt, IfStmt, WhileStmt, ForStmt, MatchStmt>;
    Data data;
    SourceSpan span{};
};

struct Parameter { std::string name; TypeName type; bool writable{}; SourceSpan span{}; ExprPtr default_value; bool is_const{}; };
struct FunctionDecl {
    std::string name;
    std::vector<Parameter> parameters;
    TypeName return_type;
    std::vector<StmtPtr> body;
    SourceSpan span{};
    bool is_override{};
    std::vector<std::string> type_parameters;
    std::optional<std::string> external_symbol;
};

struct FieldDecl {
    std::string name;
    TypeName type;
    SourceSpan span{};
    ExprPtr default_value;
    bool is_const{};
};

struct ClassDecl {
    std::string name;
    std::optional<std::string> parent;
    std::vector<FieldDecl> fields;
    std::vector<FunctionDecl> methods;
    SourceSpan span{};
    std::vector<std::string> type_parameters;
    std::optional<TypeName> parent_type;
};

struct ImportDecl {
    std::string alias;
    std::string target;
    bool local_path{};
    std::optional<std::string> extension_target;
    SourceSpan span{};
};

struct Program {
    std::string language_version;
    std::vector<ClassDecl> classes;
    std::vector<FunctionDecl> functions;
    std::vector<StmtPtr> statements;
    std::vector<ImportDecl> imports;
};

struct ResolvedProgram {
    Program program;
};

struct ConcreteProgram {
    Program program;
};

} // namespace quidra
