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

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct TypeName {
    std::string name;
    std::vector<TypeName> arguments;
    // fn<Result>(Args...) keeps parameter types separate from generic arguments.
    std::vector<TypeName> function_parameters;
    std::size_t array_depth{};
    std::vector<long long> dimensions;
    // Null means an unconstrained [] dimension. Non-null expressions are
    // evaluated once when the binding is created and then captured.
    std::vector<std::shared_ptr<Expr>> dimension_expressions;
    SourceSpan span{};
    // tensor_shape_prefix stores a source-visible exact shape pattern for
    // tensor/neural values. Nonnegative entries are fixed extents and -1 is
    // the '_' wildcard. tensor_rank and tensor_known_shape_prefix also carry
    // compiler-internal flow refinements.
    std::vector<long long> tensor_shape_prefix;
    // Null means '_' for the corresponding axis. Non-null expressions are
    // evaluated once when a binding is created.
    std::vector<std::shared_ptr<Expr>> tensor_shape_expressions;
    std::optional<long long> tensor_rank;
    std::vector<long long> tensor_known_shape_prefix;
};

struct IntegerExpr {
    std::uint64_t value{};
    std::string spelling;
    bool fits_u64{true};
};
struct FloatExpr {
    double value{};
    std::string spelling;
};
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
    std::string source_file;
    std::vector<Parameter> parameters;
    TypeName return_type;
    std::vector<StmtPtr> body;
    SourceSpan span{};
    bool is_private{};
    std::vector<std::string> type_parameters;
    std::optional<std::string> external_symbol;
    // Empty string means unconstrained. Entries align with type_parameters.
    std::vector<std::string> type_constraints;
    // A class member named `construct`. Its return type is the class itself,
    // filled in by the parser for the bare `construct(...)` spelling, or the
    // class joined with `error` when the writer spelled `T | error construct`.
    bool is_constructor{};
    // True when the writer spelled a return type before `construct`.
    bool constructor_typed{};
};

struct FieldDecl {
    std::string name;
    TypeName type;
    SourceSpan span{};
    ExprPtr default_value;
    bool is_const{};
    bool is_private{};
};

struct ClassDecl {
    std::string name;
    std::string source_file;
    std::vector<FieldDecl> fields;
    std::vector<FunctionDecl> methods;
    SourceSpan span{};
    std::vector<std::string> type_parameters;
    // Empty string means unconstrained. Entries align with type_parameters.
    std::vector<std::string> type_constraints;
};

struct EnumVariantDecl {
    std::string name;
    std::optional<TypeName> payload;
    SourceSpan span{};
};

struct EnumDecl {
    std::string name;
    std::string source_file;
    std::vector<EnumVariantDecl> variants;
    SourceSpan span{};
};

struct ImportDecl {
    std::string alias;
    std::string target;
    bool local_path{};
    // `public import`: the alias is re-exported, so an importer of this
    // module reaches the target's declarations as `module.alias.name`.
    bool is_public{};
    SourceSpan span{};
};

struct Program {
    std::string language_version;
    std::string root_source_file;
    std::vector<ClassDecl> classes;
    std::vector<EnumDecl> enums;
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
