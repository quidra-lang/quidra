#pragma once
#include "quidra/diagnostic.hpp"
#include "quidra/compiler_extension.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
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
    // tensor values. Nonnegative entries are fixed extents and -1 is
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
struct RealLiteralExpr {
    double value{};
    std::string spelling;
};
// An imaginary literal (`2.0i`): the spelling of its real-form magnitude,
// without the `i`. integer_form: written without a decimal point (`2i`),
// which the checker rejects with the real form to write.
struct ImaginaryLiteralExpr {
    std::string spelling;
    bool integer_form{};
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
// `this.NAME` inside a method or constructor body is a NameExpr whose
// this_qualifier holds the span of `this.`; the expression's own span stays
// the field name's. A qualified name only ever denotes a receiver field.
struct NameExpr {
    std::string name;
    std::optional<SourceSpan> this_qualifier{};
};
struct ArrayExpr { std::vector<ExprPtr> elements; };
struct IndexPart {
    bool slice{};
    // Direction markers are attached to the colon, never to comparisons.
    // '<' means ascending, '>' descending, 0 means no marker.
    char start_marker{};
    char end_marker{};
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
    // Construction of a class whose constructor declares type parameters of
    // its own: the name of the constructor instance the frontend selected.
    std::string constructor{};
};
struct MethodCallExpr {
    ExprPtr receiver;
    std::string method;
    std::vector<CallArg> args;
    std::vector<TypeName> type_arguments;
};
struct TryExpr { ExprPtr value; };
// `if C1 then V1 elif C2 then V2 ... else W`: conditions[i] selects values[i];
// `otherwise` is the value when no condition holds. conditions and values
// have the same size, at least one.
struct IfExpr {
    std::vector<ExprPtr> conditions;
    std::vector<ExprPtr> values;
    ExprPtr otherwise;
};

struct Expr {
    using Data = std::variant<IntegerExpr, RealLiteralExpr, ImaginaryLiteralExpr, StringExpr, StringTemplateExpr, BoolExpr, VoidExpr, NoneExpr,
                              NameExpr, ArrayExpr, IndexExpr, MemberExpr, UnaryExpr, BinaryExpr,
                              CallExpr, MethodCallExpr, TryExpr, IfExpr>;
    Data data;
    SourceSpan span{};
    // Imported package constants preserve their declaration type as a fallback
    // context. A surrounding explicit context still takes precedence, which
    // keeps context-sensitive constant expressions generic and package-owned.
    std::optional<TypeName> contextual_default_type;
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
struct MainGuardStmt {
    std::vector<StmtPtr> body;
    bool active{true};
    std::string source_file;
    // See FunctionDecl::module_namespace. An imported module's guard is merged
    // into the root statements but is checked against its own module.
    std::string module_namespace{};
};
struct WhileStmt { ExprPtr condition; std::vector<StmtPtr> body; };
struct ForStmt { std::string name; bool writable{}; ExprPtr iterable; std::vector<StmtPtr> body; };
struct MatchCase { TypeName type; std::string tag; std::optional<std::string> binder; std::vector<StmtPtr> body; SourceSpan span{}; };
struct MatchStmt { ExprPtr value; std::vector<MatchCase> cases; };

struct Stmt {
    using Data = std::variant<BindingStmt, AssignStmt, RebindStmt, ReturnStmt, LoopControlStmt,
                              ExprStmt, IfStmt, MainGuardStmt, WhileStmt, ForStmt, MatchStmt>;
    Data data;
    SourceSpan span{};
};

struct Parameter { std::string name; TypeName type; bool writable{}; SourceSpan span{}; ExprPtr default_value; bool is_const{}; };
// The `export "ABI"` prefix of a function declaration: the function is also
// callable from C. `symbol` is the C symbol, the name as declared, which the
// frontend records before it qualifies the name with the module namespace.
struct ForeignExport {
    std::string abi;
    std::string symbol;
    SourceSpan span{};
};
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
    bool is_prototype{};
    std::optional<SourceSpan> prototype_span{};
    // Module namespace the declaration was loaded under: empty for the root
    // file, "vision" or "nn.math" for imported modules. Generic instances
    // keep the namespace of their template, so name visibility inside the
    // body is decided against the declaring module's scope.
    std::string module_namespace{};
    // An `export "C"` prefix: the outbound direction of the C ABI, as
    // external_symbol is the inbound one.
    std::optional<ForeignExport> foreign_export{};
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
    bool is_prototype{};
    std::optional<SourceSpan> prototype_span{};
    // See FunctionDecl::module_namespace.
    std::string module_namespace{};
    // A class of the standard library: set where the frontend creates the
    // standard declarations and copied to the instances of a generic one.
    // Its origin, never its name, makes a class standard.
    bool standard_library{};
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
    std::string root_source_file;
    // The root file as runtime failures name it (CompileOptions::
    // source_display_path).
    std::string source_display_path;
    // Keep the exact source snapshot through lowering so runtime provenance can
    // use the same revision/node identifiers as the public inspect protocol.
    std::map<std::string, std::string> source_texts;
    // The user files, by absolute path: the root first, then every module a
    // quoted (local) import of a user file reaches, in load order. Modules
    // of installed packages, and the files a package reaches through its own
    // quoted imports, are package code and are not listed; a file reached
    // both ways is a user file. Runtime failures in package code are
    // reported at the user's statement.
    std::vector<std::string> user_sources;
    std::vector<ClassDecl> classes;
    std::vector<EnumDecl> enums;
    std::vector<FunctionDecl> functions;
    std::vector<StmtPtr> statements;
    std::vector<ImportDecl> imports;
};

struct ResolvedProgram {
    Program program;
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    // Every package the program imports, directly or through other modules:
    // its name and the absolute path of its main module, as the loader
    // resolved it.
    std::map<std::string, std::filesystem::path> packages;
};

struct ConcreteProgram {
    Program program;
    std::vector<CompilerExtensionRegistration> compiler_extensions;
};

} // namespace quidra
