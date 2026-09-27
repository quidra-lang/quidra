#pragma once
#include "quidra/ast.hpp"
#include "quidra/types.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quidra {

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
};

struct ClassFieldType {
    std::string name;
    Type type;
    std::size_t index{};
    const Expr* default_value{};
    bool is_const{};
    bool is_private{};
    std::string owner;
};

struct ClassTypeInfo {
    std::string name;
    std::vector<ClassFieldType> fields;
    std::unordered_map<std::string, std::string> methods;
    std::unordered_map<std::string, std::string> private_methods;
    // Internal function names of the class's construct(...) members, in
    // declaration order. A constructor is an ordinary function whose result is
    // the class (or the class joined with error); its receiver is a local.
    std::vector<std::string> constructors;
    std::unordered_set<std::string> private_constructors;
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

struct CheckedProgram {
    Program program;
    std::unordered_map<std::string, FunctionType> functions;
    std::unordered_map<std::string, ClassTypeInfo> classes;
    std::unordered_map<const Expr*, Type> expr_types;
    std::unordered_map<const Expr*, Type> raw_types;
    std::unordered_map<const Expr*, FieldAccessInfo> field_accesses;
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
};

class Checker {
public:
    explicit Checker(std::size_t max_errors = 20) : max_errors_(max_errors ? max_errors : 1) {}
    CheckedProgram check(ConcreteProgram program);

private:
    std::unordered_map<std::string, FunctionType> functions_;
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
    std::unordered_map<const Expr*, MethodCallInfo> method_calls_;
    std::unordered_map<const Expr*, CallResolution> call_resolutions_;
    std::unordered_map<const Expr*, std::string> function_references_;
    std::unordered_map<const Stmt*, Type> binding_types_;
    std::unordered_map<const MatchCase*, Type> case_types_;
    std::unordered_map<const MatchCase*, int> case_tags_;
    std::unordered_map<const Expr*, EnumConstructionInfo> enum_constructions_;
    std::unordered_set<const Expr*> bounds_proven_;
    std::unordered_set<const Expr*> fail_fast_expressions_;
    std::unordered_set<std::string> initialized_, narrowed_, borrowed_, const_bindings_;
    std::unordered_map<std::string, long long> const_integer_values_;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_initialized_paths_;
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
    Type current_return_{Type::simple(TypeKind::Void)};
    bool in_function_{};
    std::size_t loop_depth_{};
    std::size_t expr_depth_{};
    std::size_t stmt_depth_{};
    bool explicit_numeric_literal_context_{};
    std::string current_class_;
    // Checker-internal name of every class member body, constructors included.
    std::unordered_map<const FunctionDecl*, std::string> method_internal_names_;
    // True while checking a construct(...) body: receiver fields start out
    // uninitialized (except defaults), reads of them are errors rather than
    // requirements, and const fields may be assigned once.
    bool in_constructor_{};
    std::size_t constructor_block_depth_{};
    std::unordered_map<const Expr*, ScanFormat> scan_formats_;
    // The expression whose error cannot continue past it: a statement's
    // expression (fail-fast) or the operand of try (propagation). scan uses
    // it to decide whether its targets are initialized afterwards.
    const Expr* error_terminating_expr_{};

    Type resolve_type(const TypeName& type, bool allow_auto = false);
    void check_type_extent_expressions(const TypeName& source);
    Type check_expr(const Expr& expr, const Type* expected = nullptr);
    Type check_address_target(const Expr& expr, bool allow_tensor_element = false);
    bool storage_initialized(const Expr& expr) const;
    void check_static_index_bounds(const Type& base, const Expr& index);
    Type check_name_expr(const Expr& expression, const NameExpr& node,
                         const Type* expected = nullptr);
    Type check_member_expr(const Expr& expression, const MemberExpr& node);
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
    bool const_access_path(const Expr& expr) const;
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
    void check_rebind_stmt(const Stmt& stmt, const RebindStmt& node);
    void check_assign_stmt(const Stmt& stmt, const AssignStmt& node);
    void check_loop_control_stmt(const Stmt& stmt, const LoopControlStmt& node);
    void check_return_stmt(const Stmt& stmt, const ReturnStmt& node);
    void check_expression_stmt(const Stmt& stmt, const ExprStmt& node);
    void check_if_stmt(const Stmt& stmt, const IfStmt& node);
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
    const std::string* find_method(const std::string& class_name, const std::string& method) const;
    bool member_name_visible(const std::string& name) const;
    bool equality_supported(const Type& type) const;
    bool fully_initialized_for_equality(const Expr& expression, const Type& type) const;
    std::unordered_set<std::string> complete_class_paths(const Type& type) const;
    std::string reference_root(const std::string& name) const;
    [[noreturn]] void error(std::string code, std::string message, SourceSpan span) const;
};

} // namespace quidra
