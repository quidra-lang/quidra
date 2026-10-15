#pragma once
#include "quidra/shape_extent.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/compiler_extension.hpp"
#include "quidra/declaration_identity.hpp"
#include "quidra/slice_form.hpp"
#include <cstddef>
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
namespace semantics { struct InterfaceSources; }

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// One axis of a brace shape (tensor<real32>{nat N, 3, _}), classified on
// tokens by the parser:
//   Declare       `nat N`         a new shape variable N, the axis's extent
//   DeclareEqual  `nat N = EXPR`  a new shape variable N, equal to EXPR
//   Bare          `N`             an existing shape variable or nat value
//   Unnamed       EXPR            any other integer expression, `(n)` too
//   Discard       `_`             an extent of any length, unnamed
enum class ShapeAxisKind : std::uint8_t { Declare, DeclareEqual, Bare, Unnamed, Discard };

struct ShapeAxis {
    ShapeAxisKind kind{ShapeAxisKind::Unnamed};
    // The declared or referenced name (Declare, DeclareEqual, Bare).
    std::string name;
    // The expression (DeclareEqual, Unnamed).
    std::shared_ptr<Expr> value;
    SourceSpan span{};
};

struct TypeName {
    std::string name;
    std::vector<TypeName> arguments;
    // func<Result>(Args...) keeps parameter types separate from generic arguments.
    std::vector<TypeName> function_parameters;
    std::size_t array_depth{};
    std::vector<long long> dimensions;
    // Null means an unconstrained [] dimension. Non-null expressions are
    // evaluated once when the binding is created and then captured.
    std::vector<std::shared_ptr<Expr>> dimension_expressions;
    SourceSpan span{};
    // tensor_shape_prefix stores the explicit axes of a source shape pattern
    // for tensor values. Nonnegative entries are fixed extents and -1 is
    // the '_' wildcard. tensor_rank and tensor_known_shape_prefix also carry
    // compiler-internal flow refinements.
    std::vector<long long> tensor_shape_prefix;
    // The boundary between explicit leading and trailing axes when `...`
    // accepts any number of intervening axes. No placeholder axis is stored.
    std::optional<std::size_t> tensor_shape_ellipsis{};
    // Null means '_' for the corresponding axis. Non-null expressions are
    // evaluated once when a binding is created.
    std::vector<std::shared_ptr<Expr>> tensor_shape_expressions;
    std::optional<long long> tensor_rank;
    std::vector<ShapeExtent> tensor_known_shape_prefix;
    // A shape written in braces (tensor<real32>{nat N, 3}): its axes as
    // written. tensor_shape_prefix then holds each axis's code (a literal's
    // value, -1 for `_` and `nat N`, -2 for the other forms) and
    // tensor_shape_expressions only the literal axes; the checker gives the
    // other forms their meaning.
    bool brace_shape{};
    std::vector<ShapeAxis> shape_axes;
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
// A class member resolved by its direct source name is a NameExpr whose
// this_qualifier records its source span. The flag is internal receiver
// metadata; it does not imply an implicit source-visible instance name.
struct NameExpr {
    std::string name;
    std::optional<SourceSpan> this_qualifier{};
    // The fixed whole-instance name declared in a class header.
    bool self_reference{};
};
struct ArrayExpr { std::vector<ExprPtr> elements; };
struct TupleExpr { std::vector<ExprPtr> elements; };
// One value-array source, evaluated once before the expression-local binding.
struct ArrayComprehensionExpr {
    std::optional<TypeName> type;
    std::string name;
    SourceSpan binding_span{};
    ExprPtr source;
    ExprPtr condition;
    ExprPtr value;
};
struct IndexPart {
    bool slice{};
    // The written form of a slice's start and end (quidra/slice_form.hpp):
    // a marker is a '<' or '>' that touches the first colon, never a
    // comparison. Omitted when the side has no value (and for an index).
    SliceStart start_form{SliceStart::omitted};
    SliceEnd end_form{SliceEnd::omitted};
    ExprPtr index;
    ExprPtr start;
    ExprPtr stop;
    ExprPtr step;
    SourceSpan span{};
};
struct IndexExpr { ExprPtr base; std::vector<IndexPart> items; };
struct MemberExpr { ExprPtr base; std::string name; };
struct UnaryExpr { std::string op; ExprPtr operand; };
// op_span is the operator token's span where the parser knows it.
struct BinaryExpr { std::string op; ExprPtr left; ExprPtr right; SourceSpan op_span{}; };
// Two or more comparison operators at one level (L14): `a < b <= c` means
// `a < b and b <= c`, each operand evaluated at most once, left to right,
// short-circuiting. ops[i] joins operands[i] and operands[i + 1]; op_spans[i]
// is that operator's token. The parser keeps a single comparison a
// BinaryExpr and flattens an unparenthesized relational comparison next to
// '==' or '!=' into one chain, so the checker sees every operator of a
// mixed sequence (`a < b == c`) and rejects it.
struct ComparisonChainExpr {
    std::vector<ExprPtr> operands;
    std::vector<std::string> ops;
    std::vector<SourceSpan> op_spans;
};
struct CallArg { std::optional<std::string> name; bool writable{}; ExprPtr value; SourceSpan span{}; };
struct CallExpr {
    std::string callee;
    std::vector<CallArg> args;
    std::vector<TypeName> type_arguments;
    // An internal selected generation operation, when explicitly provided
    // by compiler-built source. Ordinary method names do not select it.
    std::string constructor{};
    // The call is the outermost call of a postfix chain written with the
    // mutation marker (L1): `.f()`, `.T()`, `.mod.f()`.
    bool mutation_marker{};
};
struct MethodCallExpr {
    ExprPtr receiver;
    std::string method;
    std::vector<CallArg> args;
    std::vector<TypeName> type_arguments;
    // See CallExpr::mutation_marker: `.obj.f()` marks the chain `obj.f()`.
    bool mutation_marker{};
};
struct TryExpr { ExprPtr value; };
// A use of a contextual constant (`const literal real NAME = ...`): the name
// as the use writes it ("math.pi", "pi") and a clone of the constant's
// initializer. The frontend puts it where the name is used. It is a literal
// of the real category: it materializes into the real type its context
// expects, as a real literal does, and has no type of its own.
struct LiteralConstantExpr {
    std::string name;
    ExprPtr value;
    // The declaration's initializer, which identifies the constant: uses of
    // one constant share its materialized values.
    const Expr* origin{};
};
// `if C1 then V1 elif C2 then V2 ... else W`: conditions[i] selects values[i];
// `otherwise` is the value when no condition holds. conditions and values
// have the same size, at least one.
struct IfExpr {
    std::vector<ExprPtr> conditions;
    std::vector<ExprPtr> values;
    ExprPtr otherwise;
};

struct TupleProjectionExpr { std::string subject; std::vector<std::size_t> indices; };
struct Expr {
    using Data = std::variant<IntegerExpr, RealLiteralExpr, ImaginaryLiteralExpr, StringExpr, StringTemplateExpr, BoolExpr, VoidExpr, NoneExpr,
                              NameExpr, ArrayExpr, IndexExpr, MemberExpr, UnaryExpr, BinaryExpr,
                              CallExpr, MethodCallExpr, TryExpr, IfExpr, LiteralConstantExpr, ComparisonChainExpr, TupleExpr, TupleProjectionExpr, ArrayComprehensionExpr>;
    Data data;
    SourceSpan span{};
    // A use of an imported typed constant (`const real32 eps = ...`) is a
    // clone of its initializer with the constant's declared type here: the
    // use has that type whatever its context, and constant_name is the name
    // as the use writes it ("mod.eps"). A constant that re-materializes in
    // its use-site type is a contextual one (LiteralConstantExpr).
    std::optional<TypeName> contextual_default_type;
    std::string constant_name;
};

template<class Node, class Visitor>
void visit_array_comprehension(Node& node, const Visitor& visit) {
    visit(*node.source);
    if (node.condition) visit(*node.condition);
    visit(*node.value);
}

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Pattern {
    enum class Kind { Tuple, Declare, Place, Discard, TypeOnly };
    Kind kind{Kind::Tuple};
    std::vector<Pattern> elements;
    std::optional<TypeName> type;
    std::string name;
    ExprPtr place;
    bool reference{};
    bool is_const{};
    SourceSpan span{};
};
template<class P, class Visit> void visit_pattern(P& pattern, Visit&& visit) {
    visit(pattern);
    for(auto& child:pattern.elements) visit_pattern(child,visit);
}
struct BindingStmt {
    TypeName declared_type;
    std::string name;
    ExprPtr value;
    bool reference{};
    bool reference_initializer{};
    bool is_const{};
    // `const literal T NAME = ...`: a contextual constant, whose uses are
    // literals (LiteralConstantExpr); it has no storage of its own.
    bool literal_constant{};
    // A top-level `private const` binding: usable only inside its module.
    bool is_private{};
    std::optional<Pattern> pattern;
    bool is_local{};
};
struct AssignStmt { ExprPtr target; ExprPtr value; std::string compound_op; std::optional<Pattern> pattern; const Expr* pattern_target{}; const Expr* target_expr() const {return pattern_target?pattern_target:target.get();} };
struct RebindStmt { std::string name; ExprPtr target; ExprPtr place; };
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
struct ParallelStmt { std::vector<StmtPtr> body; };
struct WhileStmt { ExprPtr condition; std::vector<StmtPtr> body; };
// A range's start lives in ForStmt::iterable, so header visitors share the
// same first operand as collection loops. The end and step are explicit
// operands, never a synthetic call to the legacy range builtin.
struct ForRangeHeader {
    bool start_inclusive{true};
    bool end_inclusive{};
    bool descending{};
    ExprPtr end;
    ExprPtr step;
    bool implicit_start_zero{};
    bool implicit_end_zero{};
};
struct ForIndexBinding { std::string name; SourceSpan span{}; };
struct ForCollectionBinding {
    std::string name;
    ExprPtr iterable;
    std::optional<TypeName> type;
    bool is_auto{};
    bool declares{true};
    SourceSpan span{};
};
struct ForStmt {
    std::string name;
    bool writable{};
    ExprPtr iterable;
    std::vector<StmtPtr> body;
    std::optional<ForRangeHeader> range;
    std::optional<TypeName> type;
    bool is_auto{};
    bool declares{true};
    bool legacy{true};
    std::optional<ForIndexBinding> index;
    std::optional<Pattern> pattern;
    std::vector<ForCollectionBinding> collections;
    bool parallel{};
    SourceSpan parallel_span{};
    bool discards() const { return name=="_"; }
};
template<class Loop, class Visit>
void visit_for_header(Loop& loop, Visit&& visit) {
    visit(*loop.iterable);
    for (auto& binding : loop.collections) visit(*binding.iterable);
    if (loop.range) {
        visit(*loop.range->end);
        if (loop.range->step) visit(*loop.range->step);
    }
}
struct MatchCase { TypeName type; std::string tag; std::optional<std::string> binder; std::vector<StmtPtr> body; SourceSpan span{}; std::optional<Pattern> pattern; };
struct MatchStmt { ExprPtr value; std::vector<MatchCase> cases; };

struct Stmt {
    using Data = std::variant<BindingStmt, AssignStmt, RebindStmt, ReturnStmt, LoopControlStmt,
                              ExprStmt, IfStmt, MainGuardStmt, WhileStmt, ForStmt, MatchStmt, ParallelStmt>;
    Data data;
    SourceSpan span{};
};

struct Parameter { std::string name; TypeName type; bool writable{}; SourceSpan span{}; ExprPtr default_value; bool is_const{}; bool is_local{}; bool is_extension{}; };
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
    // On a member, private to its class; on a top-level function or extern
    // declaration (`private`), private to its module.
    bool is_private{};
    std::vector<std::string> type_parameters;
    std::optional<std::string> external_symbol;
    // Empty string means unconstrained. Entries align with type_parameters.
    std::vector<std::string> type_constraints;
    // The initializer synthesized from the class header and executable body.
    // Its return type is the class, or the class joined with error.
    bool is_constructor{};
    // Retained metadata for compiler-built declarations.
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
    // Declared with the mutation marker (L1): `void .update()`,
    // `extern void .set(...)`. The function may change
    // existing state that its `&` parameters do not expose.
    bool mutation_marker{};
    std::optional<DeclarationIdentity> declaration{};
};

struct FieldDecl {
    std::string name;
    TypeName type;
    SourceSpan span{};
    ExprPtr default_value;
    bool is_const{};
    bool is_private{};
    bool reference{};
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
    // `private class`: usable only inside the module that declares it.
    bool is_private{};
    std::string self_name{};
    std::optional<SourceSpan> self_span{};
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
    // `private enum`: usable only inside the module that declares it.
    bool is_private{};
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
    // The directory of every installed package the program imports, by
    // package name: the files under it are that package's code, whose error
    // codes start with the package's prefix (abi::package_error_code_prefix).
    std::map<std::string, std::string> package_directories;
    std::vector<ClassDecl> classes;
    std::vector<EnumDecl> enums;
    std::vector<FunctionDecl> functions;
    std::vector<StmtPtr> statements;
    std::vector<ImportDecl> imports;
    // The source offset of every mutation marker the parser accepted, on a
    // definition or a call, in source order (the formatter keeps them).
    std::vector<std::size_t> mutation_markers;
    std::vector<DeclarationIdentity> declaration_catalog{};
    std::shared_ptr<const semantics::InterfaceSources> interface_sources{};
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
