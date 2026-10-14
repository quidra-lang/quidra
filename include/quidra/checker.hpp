#pragma once
#include "quidra/traversal_plan.hpp"
#include "quidra/ast.hpp"
#include "quidra/types.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quidra {

namespace semantics {
class EffectSummaries;
struct InterfaceSources;
struct BindingEntryInitializations;
}

struct FunctionParameterType {
    std::string name;
    Type type;
    bool writable{};
    const Expr* default_value{};
    bool is_const{};
};

struct StorageEffect {
    std::unordered_set<std::string> required;
    std::unordered_set<std::string> writes;
    std::unordered_set<std::string> initializes;
    std::unordered_set<std::string> invalidates;

    bool operator==(const StorageEffect&) const = default;
};

enum class CallKind {
    Function,
    FunctionValue,
    Builtin,
    NumericCast,
    Constructor
};

struct CallResolution {
    CallKind kind{CallKind::Function};
    std::string target;
    std::optional<BuiltinCallable> builtin;
    Type type{Type::simple(TypeKind::Void)};
    std::optional<Type> reflected_target{};
};

struct FunctionType {
    std::vector<FunctionParameterType> parameters;
    Type result{Type::simple(TypeKind::Void)};
    bool external{};
    // Compiler-internal control-flow summary. This is not a source-visible type.
    bool no_normal_return{};
    StorageEffect receiver_effect;
    std::unordered_map<std::string, StorageEffect> reference_effects;
    std::unordered_set<std::string> return_initialized_fields;
    std::vector<ExtentOrigin> result_extents{};
};

struct ClassFieldType {
    std::string name;
    Type type;
    std::size_t index{};
    const Expr* default_value{};
    bool is_const{};
    bool is_private{};
    std::string owner;
    bool reference{};
};

struct ClassTypeInfo {
    std::string name;
    std::vector<ClassFieldType> fields;
    std::unordered_map<std::string, std::string> methods;
    std::unordered_map<std::string, std::string> private_methods;
    // Internal function names of the class's construct(...) member: its sole
    // entry, or one per instance of a constructor that declares type
    // parameters of its own (keyed by instance name in constructor_instances).
    std::vector<std::string> constructors;
    std::unordered_map<std::string, std::string> constructor_instances;
    std::unordered_set<std::string> private_constructors;
    // ClassDecl::standard_library.
    bool standard_library{};
    std::string module_namespace;
};

struct FieldAccessInfo {
    std::string owner;
    std::size_t index{};
    Type type;
};

struct MethodCallInfo {
    std::string internal_name;
};

struct EnumConstructionInfo {
    Type type;
    int tag{};
    Type payload_type{Type::simple(TypeKind::Void)};
};

// A scan(...) call after checking: the literal text between input targets and
// the storage each target names. literals has one more entry than targets.
struct ScanFormat {
    std::vector<std::string> literals;
    std::vector<const Expr*> targets;
    std::vector<Type> target_types;
};

// A read whose initialization is known only at run time (L13): on some path
// the storage it reads is initialized and on another it is not. The read
// checks the flag that the stores of the binding declared by `binding` set,
// and fails with UNINITIALIZED naming `path`, the storage as written.
enum class InitSubject : std::uint8_t { binding, field, argument };
struct InitializationCheck {
    InitSubject subject{InitSubject::binding};
    const Stmt* binding{};
    std::string path;
};

// A contextual constant used at a fixed-width real type with an initializer
// that is not a lone literal: its value there is computed once, before the
// program's first statement, by certified rounding of `value` (the
// initializer of its first use in source order, checked as real), and every
// use loads it.
struct LiteralConstantSlot {
    std::string name;
    const Expr* value{};
    Type type;
    SourceSpan first_use{};
};

// A brace shape after checking (L17): the shape variables a binding or a
// match case declares and the check of each axis, in the order the lowering
// evaluates them. A shape variable is a read-only nat local.
struct ShapeVariable {
    std::string name;
    // The axis that declares it.
    std::size_t axis{};
    // `nat N = EXPR`: the expression; null for `nat N`, whose value is the
    // axis's extent.
    const Expr* value{};
    // Something reads it (a body, an axis expression, an equality axis, a
    // later store into the binding); an unread `nat N` costs nothing.
    bool read{};
};

enum class ShapeAxisCheck : std::uint8_t {
    // `_`, and the declaring axis of `nat N`: nothing to check.
    None,
    // The axis equals the value of `value`: a literal, an unnamed
    // expression, the expression of `nat N = EXPR` or the bare name of an
    // existing nat value.
    Value,
    // The axis equals the shape variable `variable` of the same shape: a
    // bare repeat (`{nat K, K}`).
    Variable,
};

struct ShapeAxisPlan {
    ShapeAxisCheck check{ShapeAxisCheck::None};
    const Expr* value{};
    // The operand evaluated for `value`: nat(e) over an integer e evaluates
    // e, which is how an extent was always captured (the value is checked
    // to be non-negative with the extent).
    const Expr* capture{};
    // The expression reads a shape variable of the same shape, so it is
    // evaluated after the initializer; otherwise before it.
    bool after_initializer{};
    std::vector<std::size_t> dependencies;
    // The shape variable the axis is checked against, named in the failure:
    // the unit's own (Variable, and Value for `nat N = EXPR`) or an existing
    // shape variable named bare; empty for other values.
    std::string variable;
    // Variable, and Value for `nat N = EXPR`: the index into variables.
    std::size_t variable_index{};
    // The extent is a compile-time constant (also folded into the type's
    // tensor_shape_prefix).
    std::optional<long long> constant{};
};

struct ShapePlan {
    std::vector<ShapeAxisPlan> axes;
    std::vector<ShapeVariable> variables;
    // Explicit axes before this boundary align with the head; the rest
    // align with the tail. The omitted middle contains zero or more axes.
    std::optional<std::size_t> ellipsis{};
    // The axes whose expression is evaluated after the initializer (a
    // `nat N = EXPR` or an unnamed expression over the shape's own
    // variables), in dependency order, left to right among independent ones.
    std::vector<std::size_t> after_order;
    // A match case whose selection depends on run-time tests (an equality
    // axis, a non-constant expression, or extents read from a value whose
    // rank is not known): it is taken only when they hold, and it covers
    // nothing for MATCH_EXHAUSTIVE.
    bool conditional{};
    // The rank of the value is not known to be the shape's.
    bool rank_unknown{};
};

struct SignatureShapePart {
    const TypeName* type{};
    // Zero is the result; subsequent parts identify written parameters.
    std::size_t parameter{};
    std::size_t offset{};
    std::size_t size{};
    std::string subject;
    std::optional<std::size_t> ellipsis{};
};

struct SignatureShapeUnit {
    // The aggregate copies axis records and shares their original expressions.
    std::shared_ptr<TypeName> source;
    std::vector<SignatureShapePart> parts;
};

struct ProvenShapeConstraints {
    bool rank{};
    std::vector<bool> axes;
    // Call-entry proofs remain separate from the call result's contract.
    std::vector<ProvenShapeConstraints> parameters;
};

struct PatternExpansion {
    std::string subject;
    Type type;
    std::vector<StmtPtr> bindings;
};
struct CheckedProgram {
    Program program;
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    std::unordered_map<std::string, FunctionType> functions;
    std::unordered_map<std::string, ClassTypeInfo> classes;
    std::unordered_map<const Expr*, Type> expr_types;
    std::unordered_map<const Expr*, Type> raw_types;
    std::unordered_map<const Expr*, FieldAccessInfo> field_accesses;
    std::unordered_set<const Expr*> tensor_grad_accesses;
    std::unordered_map<const Expr*, MethodCallInfo> method_calls;
    std::unordered_map<const Expr*, CallResolution> call_resolutions;
    std::unordered_map<const Expr*, std::string> function_references;
    std::unordered_map<const Stmt*, Type> binding_types;
    std::unordered_map<const MatchCase*, Type> case_types;
    std::unordered_map<const MatchCase*, int> case_tags;
    std::unordered_map<const Expr*, EnumConstructionInfo> enum_constructions;
    std::unordered_set<const Expr*> bounds_proven;
    std::unordered_set<const Expr*> fail_fast_expressions;
    std::unordered_map<const Expr*, std::unordered_set<std::string>> class_expr_initialized_paths;
    std::unordered_map<const Expr*, ScanFormat> scan_formats;
    // What every function may read, write and use (src/semantics), computed
    // at the end of the check.
    std::shared_ptr<const semantics::EffectSummaries> effects;
    // An existing collection/range variable receives its first value only
    // when a loop body is entered, never on the zero-iteration path.
    std::unordered_map<const ForStmt*,const Stmt*> iteration_initializes;
    struct ParallelForPlan {
        std::vector<std::string> captures, outputs;
        std::unordered_map<std::string,const Stmt*> capture_bindings;
        std::vector<const Expr*> pure_calls;
        bool worker_arithmetic{};
    };
    std::unordered_map<const ForStmt*,ParallelForPlan> parallel_for_plans;
    std::unordered_map<const ForCollectionBinding*,const Stmt*> collection_iteration_initializes;
    std::unordered_map<const Expr*,TraversalPlan> traversal_plans;
    std::unordered_map<std::string,const FunctionDecl*> traversal_functions;
    std::unordered_map<const Expr*,TraversalBinding> traversal_bindings;
    // Initialization checked at run time (L13): the reads that check a flag,
    // the binding declarations that keep one (those some check reads), and
    // where a binding that may be uninitialized becomes initialized: after a
    // simple statement (an assignment, a binding, an expression statement)
    // or after a call expression (a `&` argument or a receiver the callee
    // initializes, a scan target). The lowering sets a binding's flag at the
    // places listed for it when the binding keeps one.
    std::unordered_map<const Expr*, InitializationCheck> initialization_checks;
    std::unordered_set<const Stmt*> initialization_flags;
    // The classes some field check reads (subject field): their values keep
    // a hidden word of initialization bits, one per field, set by every
    // store of the field.
    std::unordered_set<std::string> initialization_masked_classes;
    std::unordered_map<const Stmt*, std::vector<const Stmt*>> statement_initializes;
    std::unordered_map<const Expr*, std::vector<const Stmt*>> expression_initializes;
    // The member reads of error values (e.code, e.message, e.cause, e.path,
    // ...): the member's name.
    std::unordered_map<const Expr*, std::string> error_members;
    // The materialized values of contextual constants, and the uses that
    // load them (index into literal_constant_slot_list). A library, which
    // has no entry to compute them in, has none: its uses round at the use.
    std::vector<LiteralConstantSlot> literal_constant_slot_list;
    std::unordered_map<const Expr*, std::size_t> literal_constant_slots;
    // The direction of each array or string slice (the indexing expression,
    // or a slice assignment's target).
    std::unordered_map<const Expr*, SliceDirection> slice_directions;
    // z.real (0) and z.imag (1) of a complex value.
    std::unordered_map<const Expr*, int> complex_parts;
    // The brace shapes of bindings and match cases (L17), by declared type.
    std::unordered_map<const TypeName*, ShapePlan> shape_plans;
    // Array extents written nat(e) over an integer e: the operand captured
    // in its place (an extent was always captured as its integer value).
    std::unordered_map<const Expr*, const Expr*> extent_capture_operands;
    std::unordered_map<const FunctionDecl*, SignatureShapeUnit> signature_shapes;
    std::unordered_map<const Expr*, bool> complete_signature_checks;
    std::unordered_map<const Expr*, ProvenShapeConstraints> proven_shape_constraints;
    std::unordered_map<const TypeName*, SignatureShapeUnit> function_shape_units;
    std::unordered_map<const Stmt*, PatternExpansion> pattern_expansions;
    std::unordered_map<std::string,Type> pattern_subject_types;
    std::unordered_map<const Pattern*,PatternExpansion> control_patterns;
    std::unordered_map<const Expr*, std::string> runtime_bound_checks;
    std::unordered_map<const Expr*,std::vector<std::vector<std::size_t>>> stored_target_initialization_checks;
    std::unordered_map<const Expr*,std::unordered_set<std::string>> reference_invalidations;
    std::shared_ptr<const semantics::InterfaceSources> interface_sources{};
    std::shared_ptr<const semantics::BindingEntryInitializations> binding_entry_initialization{};
};

// What a compilation produces: an executable (quidra build, run, a direct
// run), a static library for a C host (quidra build --lib), or a REPL
// submission. A library's root is checked like an imported module and has
// no entry point; a REPL submission cannot export C symbols.
enum class CompileArtifact { Executable, Library, Interactive };

class Checker {
public:
    explicit Checker(std::size_t max_errors = 20,
                     CompileArtifact artifact = CompileArtifact::Executable)
        : max_errors_(max_errors ? max_errors : 1), artifact_(artifact) {}
    CheckedProgram check(ConcreteProgram program);

private:
    std::unordered_map<std::string, FunctionType> functions_;
    std::shared_ptr<semantics::BindingEntryInitializations> binding_entry_initialization_;
    std::unordered_map<std::string, ClassTypeInfo> classes_;
    std::unordered_set<std::string> class_names_;
    std::unordered_map<std::string, Type> enum_types_;
    std::unordered_map<std::string, Type> variables_;
    std::unordered_map<std::string, std::string> reference_roots_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> reference_paths_;
    std::unordered_set<std::string> unknown_reference_targets_;
    std::unordered_map<const Expr*, Type> expr_types_;
    std::unordered_map<const Expr*, Type> raw_types_;
    std::unordered_map<const Expr*, FieldAccessInfo> field_accesses_;
    std::unordered_set<const Expr*> tensor_grad_accesses_;
    std::unordered_map<const Expr*, MethodCallInfo> method_calls_;
    std::unordered_map<const Expr*, CallResolution> call_resolutions_;
    std::unordered_map<const Expr*, std::string> function_references_;
    std::unordered_map<const Stmt*, Type> binding_types_;
    std::unordered_map<const Stmt*, PatternExpansion> pattern_expansions_;
    std::unordered_map<const Pattern*,PatternExpansion> control_patterns_;
    std::unordered_map<const MatchCase*, Type> case_types_;
    std::unordered_map<const MatchCase*, int> case_tags_;
    std::unordered_map<const Expr*, EnumConstructionInfo> enum_constructions_;
    std::unordered_set<const Expr*> bounds_proven_;
    std::unordered_map<const Expr*, SliceDirection> slice_directions_;
    std::unordered_set<const Expr*> fail_fast_expressions_;
    std::unordered_set<std::string> initialized_, narrowed_, borrowed_, const_bindings_;
    // L13: bindings that are initialized on some path to here but not on
    // every one (initialized_ holds those initialized on every path); a
    // binding in neither is uninitialized on every path.
    std::unordered_set<std::string> maybe_initialized_;
    // Bindings that a call may have written without guaranteeing their
    // initialization (a `&` argument or receiver whose callee writes on some
    // paths only, a write through a reference whose target is not known),
    // so that no flag tracks them; a read that is not initialized on every
    // path stays a compile error for them. It only grows within a body.
    std::unordered_set<std::string> untracked_initialized_;
    // Generation entries with no retained untracked initialization state.
    std::unordered_set<std::string> mask_tracked_constructors_;
    // The run-time checks in the order they were recorded, with the binding
    // each reads, so that a loop can turn the checks of its body back into
    // errors when the body writes their binding untracked.
    std::vector<std::pair<const Expr*, std::string>> initialization_check_order_;
    // L13 for array elements: the one-dimensional local arrays created
    // without element values (`T[n] a`, `T[] a = array(k)`), by name, with
    // the number of elements tracked one by one (n or k when constant and at
    // most 256), or -1 when one state stands for every element. An element
    // that may be initialized is listed in maybe_initialized_ as "a[i]", and
    // "a[]" stands for every element; a read of an element that no path
    // initializes is a compile error, everything else is checked at run
    // time per element as before.
    std::unordered_map<std::string, long long> element_tracked_arrays_;
    // The target of the element assignment being checked, whose store is
    // recorded once its value is checked.
    const Expr* element_store_target_{};
    // The declaration of each local binding name, the latest one checked.
    std::unordered_map<std::string, const Stmt*> binding_declarations_;
    // The simple statement and the call expression being checked, where a
    // binding becomes initialized.
    const Stmt* current_simple_statement_{};
    const Expr* current_call_expression_{};
    std::unordered_map<const Expr*, InitializationCheck> initialization_checks_;
    std::unordered_map<const Expr*, std::string> runtime_bound_checks_;
    std::unordered_map<const Expr*,std::vector<std::vector<std::size_t>>> stored_target_initialization_checks_;
    std::unordered_map<const Expr*,std::unordered_set<std::string>> reference_invalidations_;
    void check_reference_bound(const Expr& expression, const std::string& name);
    void check_field_reference_bound(const Expr& expression, const ClassFieldType& field,
                                     const Expr* object = nullptr);
    std::unordered_map<const ForStmt*,const Stmt*> iteration_initializes_;
    std::unordered_map<const ForStmt*,CheckedProgram::ParallelForPlan> parallel_for_plans_;
    void validate_parallel_for(const Stmt&,const ForStmt&,const std::unordered_map<std::string,Type>&);
    std::unordered_map<const ForCollectionBinding*,const Stmt*> collection_iteration_initializes_;
    std::unordered_map<const Expr*,TraversalPlan> traversal_plans_;
    std::unordered_map<std::string,const FunctionDecl*> traversal_functions_;
    std::unordered_map<const Expr*,TraversalBinding> traversal_bindings_;
    std::unordered_map<const Expr*,bool> traversal_forms_;
    TraversalBinding traversal_binding(const Expr& expression);
    std::vector<std::pair<std::string,std::vector<std::string>>> traversal_borrows_;
    // The visit proxy keeps local facts while writes reach every real candidate.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
        traversal_reference_origins_;
    void apply_traversal_storage_effect(const Expr& target, const StorageEffect& effect);
    std::unordered_map<std::string, std::unordered_set<std::string>>
        traversal_element_invalidations_;
    std::unordered_map<const Stmt*, std::vector<const Stmt*>> statement_initializes_;
    std::unordered_map<const Expr*, std::vector<const Stmt*>> expression_initializes_;
    std::unordered_map<std::string, long long> const_integer_values_;
    // New range variables and collection indices are fixed by their header.
    std::unordered_map<std::string, bool> loop_readonly_;
    std::vector<const Expr*> live_iteration_paths_;
    std::unordered_set<std::string> loop_bit_places_;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_initialized_paths_;
    // The classes whose fields some run-time check reads.
    std::unordered_set<std::string> initialization_masked_classes_;
    std::unordered_map<const Expr*, std::unordered_set<std::string>> class_expr_initialized_paths_;
    StorageEffect current_receiver_effect_;
    std::unordered_set<std::string> current_return_initialized_;
    bool current_return_summary_seen_{};
    std::unordered_set<std::string> current_reference_parameters_;
    std::unordered_map<std::string, StorageEffect> current_reference_effects_;
    std::unordered_set<std::string> current_exit_receiver_initialized_;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        current_exit_reference_initialized_;
    bool current_effect_exit_summary_seen_{};
    std::unordered_set<const FunctionDecl*> invalid_functions_;
    std::vector<Diagnostic> diagnostics_;
    bool suppress_diagnostics_{};
    std::size_t max_errors_{20};
    CompileArtifact artifact_{CompileArtifact::Executable};
    void check_library_root(const Program& program);
    Type current_return_{Type::simple(TypeKind::Void)};
    bool in_function_{};
    std::size_t loop_depth_{};
    std::size_t parallel_node_depth_{};
    std::size_t expr_depth_{};
    std::size_t stmt_depth_{};
    bool explicit_numeric_literal_context_{};
    // Inside a complex literal (real and imaginary literals joined): a real
    // literal is the real part of a value of the context's complex type.
    bool complex_literal_context_{};
    std::unordered_map<const Expr*, int> complex_part_accesses_;
    std::string current_class_;
    // Module namespace of the body being checked ("" for the root file). Local
    // names are checked against the declarations of this module only.
    std::string current_module_namespace_;
    // The code prefix of the package whose code is being checked (empty in
    // user code), and what decides it: the user files and the packages'
    // directories.
    std::string current_error_code_prefix_;
    std::string current_root_source_file_;
    std::unordered_set<std::string> user_source_files_;
    std::map<std::string, std::string> package_directories_;
    std::unordered_map<const Expr*, std::string> error_members_;
    // Sets current_error_code_prefix_ for code of `source_file`.
    void enter_error_code_scope(const std::string& source_file);
    Type check_error_constructor(const Expr& expression, const CallExpr& node);
    Type check_error_member(const Expr& expression, const MemberExpr& node);
    // Functions declared by the root file, generic instantiations of its
    // templates included. They are not in the lexical environment of code in
    // an imported module (see visible_function_name).
    std::unordered_set<std::string> root_functions_;
    // Checker-internal name of every class member body, constructors included.
    std::unordered_map<const FunctionDecl*, std::string> method_internal_names_;
    // True while checking a construct(...) body: receiver fields start out
    // uninitialized (except defaults), reads of them are errors rather than
    // requirements, and const fields may be assigned once.
    bool in_constructor_{};
    // True while checking a constructor's parameter shapes: no receiver
    // exists at its entry, so `this.NAME` is rejected there.
    bool this_unavailable_{};
    std::size_t constructor_block_depth_{};
    std::unordered_map<const Expr*, ScanFormat> scan_formats_;
    std::vector<LiteralConstantSlot> literal_constant_slot_list_;
    std::unordered_map<const Expr*, std::size_t> literal_constant_slots_;
    std::map<std::pair<const Expr*, std::string>, std::size_t> literal_constant_slot_index_;
    // L1 option A: the const values passed to a call, by value or as its
    // receiver, whose shared state the callee may change; decided once the
    // effect summaries exist (parameter 0 of a method is its receiver).
    struct ConstArgumentUse {
        const Expr* call{};
        std::size_t parameter{};
        const Expr* argument{};
    };
    std::vector<ConstArgumentUse> const_argument_uses_;
    void check_const_argument_uses(const CheckedProgram& checked);
    // L17: the brace shapes checked so far, the visible shape variables by
    // name (their shape and index), the binding that declared each shape
    // (for its later stores), the brace shape whose axes may take every
    // form while its declaration is checked, and the array extents nat(e).
    std::unordered_map<const TypeName*, ShapePlan> shape_plans_;
    std::unordered_map<const FunctionDecl*, SignatureShapeUnit> signature_shapes_;
    struct ShapeVariableRef {
        ShapePlan* plan{};
        std::size_t index{};
        SourceSpan span{};
    };
    std::unordered_map<std::string, ShapeVariableRef> shape_variables_;
    const TypeName* brace_shape_unit_{};
    std::unordered_set<const Expr*> legacy_signature_axes_;
    bool signature_shape_resolution_{};
    bool signature_shape_unit_{};
    std::unordered_map<std::string, std::string> signature_parameter_keys_;
    std::unordered_map<const TypeName*, SignatureShapeUnit> function_shape_units_;
    std::unordered_map<std::string, ShapePlan*> shape_binding_plans_;
    std::unordered_map<const Expr*, const Expr*> extent_capture_operands_;
    // The expression whose error cannot continue past it: a statement's
    // expression (fail-fast) or the operand of try (propagation). scan uses
    // it to decide whether its targets are initialized afterwards.
    const Expr* error_terminating_expr_{};

    bool traversal_result_type_{};
    Type resolve_result_type(const TypeName& type);
    Type resolve_type(const TypeName& type, bool allow_auto = false);
    void check_type_extent_expressions(const TypeName& source);
    // L17 (checker_shapes.cpp): a brace shape of a binding or a match case,
    // checked as one scope unit; `type` gets the constant extents and the
    // identity key. declare_shape_variables makes the unit's variables
    // visible after its declaration.
    ShapePlan& check_brace_shape(const TypeName& source, Type& type, bool has_value);
    void declare_shape_variables(const TypeName& source, const ShapePlan& plan);
    void check_signature_shape(const FunctionDecl& source, FunctionType& type,
                               std::size_t parameter_offset);
    void bind_signature_shape_variables(const FunctionDecl& source);
    void check_function_type_shape(const TypeName& source, Type& type);
    Type check_shape_axis_expression(const Expr& expression);
    void resolve_bare_axis_name(const ShapeAxis& axis);
    void reject_visible_shape_variable_name(const std::string& name, SourceSpan span);
    void reject_transitional_brace_axes(const TypeName& source) const;
    void note_shape_variable_read(const std::string& name);
    bool is_shape_variable(const std::string& name) const;
    void forget_shape_variable(const std::string& name);
    // An array extent: nat(e) over an integer e captures e (Q64), so its
    // type is nat rather than nat | error.
    void note_extent_capture_operand(const Expr& expression);
    void settle_extent_conversion(const Expr& expression, Type& type);
    Type check_expr(const Expr& expr, const Type* expected = nullptr);
    Type check_if_expr(const Expr& expression, const IfExpr& node, const Type* expected);
    // L14: a chain of comparisons in one direction (`0 <= i < n`, `a == b == c`).
    Type check_comparison_chain(const Expr& expression, const ComparisonChainExpr& node);
    Type check_address_target(const Expr& expr, bool allow_tensor_element = false);
    bool storage_initialized(const Expr& expr) const;
    // L13: a read of the binding `name` (expression `read`) that is not
    // initialized on every path: a compile error when it is uninitialized on
    // every path or its storage is not known, a run-time check otherwise.
    void check_maybe_initialized_read(const Expr& read, const std::string& name);
    // L13 for class fields: a read of the field path `path` of the local
    // binding `root` (or, with root "$this", of the receiver in a
    // constructor) that is not initialized on every path, spelled `shown`.
    // Returns true when it is checked at run time (the path is initialized
    // afterwards) or reported as uninitialized on every path, false when the
    // caller reports it (untracked paths, and the receiver's untouched
    // fields, which keep the constructor's text).
    bool check_maybe_initialized_field(const Expr& read, const std::string& root,
                                       const std::string& path, const std::string& shown);
    // Records direct stores and untracked writes of field paths (L13).
    void note_field_store(const std::string& root, const std::string& path);
    void note_untracked_field_write(const std::string& root, const std::string& path);
    // A class value assigned whole to `root` (".path" below it when not
    // empty): its field paths not initialized on every path are untracked.
    void note_class_value(const std::string& root, const std::string& path, const Type& type,
                          const std::unordered_set<std::string>& initialized,
                          const Expr* source = nullptr);
    // The binding whose storage `name` designates (itself, or a reference's
    // whole target binding), or nullopt when that is not one known binding.
    std::optional<std::string> initialization_root(const std::string& name) const;
    // Records that the binding `root` becomes initialized at the current
    // simple statement and call expression, when it may be uninitialized.
    void record_initialization(const std::string& root);
    // Moves the bindings that a loop body may initialize from uninitialized
    // to maybe-initialized.
    void note_loop_initializations(const std::vector<StmtPtr>& body);
    // Reports the checks recorded since `first` whose binding the loop body
    // just checked may write untracked, as compile errors.
    void reject_untracked_loop_checks(std::size_t first);
    // Array elements (L13): starts or stops tracking the elements of `name`
    // at its declaration; records a store into the element `target`
    // (`a[i] = value`) or a write that may reach any element of the array a
    // place is rooted in; rejects a read of an element of `base` (index
    // `index`, or every element for a whole-array read) that no path
    // initializes.
    void declare_array_elements(const std::string& name, const Type& type, const BindingStmt& node);
    void note_element_store(const Expr& target);
    void note_element_writes(const Expr& place);
    void check_element_read(const Expr& base, const Expr* index, SourceSpan span);
    // The maybe-initialized bindings after a join of the continuing paths'
    // states (initialized and maybe-initialized sets).
    void join_maybe_initialized(
        const std::vector<std::pair<const std::unordered_set<std::string>*,
                                    const std::unordered_set<std::string>*>>& continuing);
    void check_static_index_bounds(const Type& base, const Expr& index);
    // An index operand: an integer of any kind (an integer literal
    // materializes as int); the bounds check takes it as it is.
    Type check_index_operand(const Expr& index);
    // A slice item's direction: from its markers, else a constant
    // step's sign, else ascending (or the sign of a run-time step). Two
    // markers that disagree, a constant step 0 and a constant step against a
    // marker are SLICE_STEP. Checks the start, end and step operands.
    void check_slice_item(const IndexPart& item);
    SliceDirection slice_item_direction(const IndexPart& item) const;
    // Constant slice operands against a statically known length (a fixed
    // array): INDEX_BOUNDS by the slice bound rules.
    void check_static_slice_bounds(const Type& base, const IndexPart& item, SourceSpan span);
    void check_static_slice_bounds(std::uint64_t length, const IndexPart& item, SourceSpan span);
    // The number of elements a slice selects when its operands are constant
    // (with an omitted side, only against a statically known length).
    std::optional<std::uint64_t> static_slice_count(const Type& base, const IndexPart& item) const;
    // A slice of an array or a string as an assignment target (L6).
    Type check_slice_assignment(const AssignStmt& node, const IndexExpr& target, const Type& base);
    Type check_name_expr(const Expr& expression, const NameExpr& node,
                         const Type* expected = nullptr);
    Type check_member_expr(const Expr& expression, const MemberExpr& node);
    Type check_complex_literal_part(const Expr& expression, const Type& complex);
    Type check_array_comprehension(const Expr& expression, const ArrayComprehensionExpr& node,
                                   const Type* expected);
    Type check_index_expr(const Expr& expression, const IndexExpr& node);
    Type check_method_call_expr(const Expr& expression, const MethodCallExpr& node,
                                const Type* expected = nullptr);
    Type check_call_expr(const Expr& expression, const CallExpr& node, const Type* expected);
    Type check_builtin_call_expr(const Expr& expression, const CallExpr& node,
                                 BuiltinCallable builtin, const Type* expected);
    std::unordered_set<std::string> initialized_paths_for_expr(const Expr& expr) const;
    std::optional<std::pair<std::string, std::string>> member_storage_path(const Expr& expr) const;
    std::optional<std::pair<std::string, std::string>> writable_storage_path(const Expr& expr) const;
    std::optional<std::pair<std::string, std::string>> alias_storage_path(const Expr& expr) const;
    bool unknown_reference_access_path(const Expr& expr) const;
    bool const_access_path(const Expr& expr, bool for_write = true) const;
    bool stable_addressable_storage(const Expr& expr) const;
    bool stable_writable_storage(const Expr& expr) const;
    std::optional<std::string> current_receiver_path(const Expr& expr) const;
    std::optional<std::pair<std::string, std::string>>
    current_reference_parameter_path(const Expr& expr) const;
    void mark_member_initialized(const Expr& expr);
    void record_storage_assignment(StorageEffect& effect, const std::string& path,
                                   const Type& type, const Expr& value);
    void record_current_receiver_assignment(const std::string& path, const Type& type, const Expr& value);
    void compose_storage_effect(StorageEffect& destination, const StorageEffect& source,
                                const std::string& prefix = {});
    struct PendingReferenceEffect {
        const Expr* target{};
        const FunctionParameterType* parameter{};
        SourceSpan span{};
    };
    bool check_call_arguments(const Expr& expression, const std::vector<CallArg>& args,
                              const FunctionType& function,
                              std::vector<PendingReferenceEffect>& pending);
    Type check_class_construction(const Expr& expression, const CallExpr& node,
                                  const std::string& class_name);
    std::unordered_set<std::string> default_initialized_paths(const std::string& class_name) const;
    FunctionType* begin_member_body(const ClassDecl& class_decl, const FunctionDecl& method);
    void finish_member_body(const ClassDecl& class_decl, const FunctionDecl& method,
                            FunctionType& signature, bool report);
    void apply_current_method_summary(const FunctionType& signature, const std::string& prefix = {});
    void apply_method_effects(const Expr& receiver, const FunctionType& signature, SourceSpan span);
    void mark_storage_initialized(const Expr& expr);
    void check_reference_effect_requirements(const Expr& target, const FunctionType& signature,
                                             const FunctionParameterType& parameter, SourceSpan span);
    void apply_reference_effect_postconditions(const Expr& target, const FunctionType& signature,
                                               const FunctionParameterType& parameter, SourceSpan span,
                                               bool allow_initializes);
    void finish_call_effects(const FunctionType& signature,
                             const std::vector<PendingReferenceEffect>& pending,
                             const Expr* receiver = nullptr,
                             bool current_receiver = false,
                             SourceSpan receiver_span = {});
    void check_storage_effect_requirements(const Expr& target, const StorageEffect& effect,
                                           SourceSpan span, bool receiver_context = false);
    void apply_storage_effect_postconditions(const Expr& target, const StorageEffect& effect,
                                             bool allow_initializes = true);
    void apply_storage_effect_to_target(const Expr& target, const StorageEffect& effect,
                                        SourceSpan span, bool receiver_context = false);
    void record_effect_exit();
    void finalize_receiver_effects(FunctionType& signature, bool include_fallthrough);
    void finalize_reference_effects(FunctionType& signature, bool include_fallthrough);
    void reset_current_effect_state();
    void check_stmt(const Stmt& stmt);
    void check_binding_stmt(const Stmt& stmt, const BindingStmt& node);
    void check_control_pattern(const Pattern&,const Type&,bool reference,bool match_case,const Expr* source);
    void check_pattern_binding(const Stmt& stmt, const BindingStmt& node);
    void check_rebind_stmt(const Stmt& stmt, const RebindStmt& node);
    void check_assign_stmt(const Stmt& stmt, const AssignStmt& node);
    void check_pattern_assignment(const Stmt& stmt,const AssignStmt& node);
    void check_loop_control_stmt(const Stmt& stmt, const LoopControlStmt& node);
    void check_return_stmt(const Stmt& stmt, const ReturnStmt& node);
    void check_expression_stmt(const Stmt& stmt, const ExprStmt& node);
    void check_if_stmt(const Stmt& stmt, const IfStmt& node);
    void check_parallel_stmt(const Stmt& stmt, const ParallelStmt& node);
    void check_while_stmt(const Stmt& stmt, const WhileStmt& node);
    void check_for_stmt(const Stmt& stmt, const ForStmt& node);
    void check_match_stmt(const Stmt& stmt, const MatchStmt& node);
    void check_block(const std::vector<StmtPtr>& body);
    void record(const CompileError& error);
    bool expr_has_no_normal_return(const Expr& expression) const;
    bool stmt_always_terminates(const Stmt& stmt) const;
    bool block_always_terminates(const std::vector<StmtPtr>& body) const;
    bool block_contains_return(const std::vector<StmtPtr>& body) const;
    const ClassFieldType* find_field(const std::string& class_name, const std::string& field) const;
    // The receiver field that `this.NAME` denotes inside a member body; null
    // for anything else. A bare name never denotes a field. Every check that
    // asks whether an expression is a receiver field goes through it.
    const ClassFieldType* receiver_field(const Expr& expression) const;
    const ClassFieldType& check_this_field(const Expr& expression, const NameExpr& name);
    void reject_bare_field(const std::string& name, SourceSpan span) const;
    void reject_bare_storage_root(const Expr& expression) const;
    bool standard_library_class(const std::string& class_name) const;
    const std::string* find_method(const std::string& class_name, const std::string& method) const;
    bool member_name_visible(const std::string& name) const;
    bool declaration_name_visible(const std::string& name) const;
    bool type_name_declared_in(const std::string& name, const std::string& module_namespace) const;
    std::optional<std::string> visible_function_name(const std::string& name) const;
    bool equality_supported(const Type& type) const;
    void check_reference_equality_bound(const Expr& expression,const Type& type);
    bool fully_initialized_for_equality(const Expr& expression, const Type& type) const;
    void check_stored_target_complete(const Expr&,const Type&);
    bool reaches_reference_field(const Expr&) const;
    std::unordered_set<std::string> complete_class_paths(const Type& type) const;
    std::string reference_root(const std::string& name) const;
    [[noreturn]] void error(std::string code, std::string message, SourceSpan span) const;
    Type check_literal_constant(const Expr& expression, const LiteralConstantExpr& node,
                                const Type* expected);
};

} // namespace quidra
