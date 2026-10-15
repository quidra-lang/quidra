#include "quidra/pattern_selection.hpp"
#include "quidra/frontend.hpp"

#include "quidra/abi/symbols.hpp"
#include "quidra/compile_inputs.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/import_path.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include "quidra/member_function_names.hpp"
#include "quidra/numeric_types.hpp"
#include "quidra/parser.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/project.hpp"
#include "quidra/toml_subset.hpp"
#include "quidra/standard_classes.hpp"
#include "quidra/types.hpp"

#include "nesting_budget.hpp"
#include "shape_relations.hpp"
#include "compiler_extension_binding.hpp"
#include "semantics/interface_contracts.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quidra {
namespace {

// The numeric type a type name spells (an alias such as int64 included), or
// nullptr (quidra/numeric_types.hpp).
const NumericKindInfo* numeric_type_name(std::string_view name) {
    const auto canonical = canonical_builtin_type_name(name);
    return canonical ? numeric_kind_info(*canonical) : nullptr;
}

// A type name with a built-in type's alias replaced by its canonical
// spelling.
std::string_view canonical_type_name(std::string_view name) {
    return canonical_builtin_type_name(name).value_or(name);
}

// Whether a type name spells the element type of a tensor shape (int or
// nat).
bool names_shape_element(std::string_view name) {
    const auto* numeric = numeric_type_name(name);
    return numeric && (numeric->kind == TypeKind::Int || numeric->kind == TypeKind::Nat);
}

// The runtime handle classes that the equality constraint excludes. It lists
// four of the five (standard_class::is_runtime_handle): autograd.Target is
// missing, a known defect that a separate fix corrects; until then the set
// keeps its contents.
bool is_unequatable_handle_without_autograd_target(std::string_view name) {
    return name == standard_class::json_value || name == standard_class::http_response ||
           name == standard_class::file_handle;
}

namespace fs = std::filesystem;

[[noreturn]] void frontend_error(std::string code, std::string message, SourceSpan span = {}) {
    throw CompileError(Diagnostic{std::move(code), std::move(message), span});
}

std::string read_text(const fs::path& path, InputFileKind kind, CompileInputs* inputs) {
    auto text = read_input_file(path, kind, inputs);
    if (!text) throw std::runtime_error("cannot read Quidra source: " + path.string());
    return std::move(*text);
}

std::string qualify(const std::string& ns, const std::string& name) {
    return ns.empty() ? name : ns + "." + name;
}

std::string first_segment(const std::string& name) {
    const auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string suffix_after_first(const std::string& name) {
    const auto dot = name.find('.');
    return dot == std::string::npos ? std::string{} : name.substr(dot + 1);
}

struct Exports {
    std::unordered_map<std::string,std::string> pseudo_types;
    std::unordered_map<std::string, std::string> classes;
    std::unordered_map<std::string, std::string> enums;
    std::unordered_map<std::string, std::string> functions;
    std::unordered_map<std::string, std::string> values;
    // Imported immutable top-level constants are side-effect-free expressions
    // cloned at qualified use sites. Packages own their values; Core owns only
    // this generic module/export mechanism.
    std::unordered_map<std::string, std::shared_ptr<Expr>> constants;
    // `public import alias = ...` re-exports the target under `alias`, so an
    // importer reaches its declarations as `module.alias.name`.
    std::unordered_map<std::string, std::shared_ptr<Exports>> namespaces;
    // The module's `private` declarations (L3), by name. They are not
    // exported, so no lookup finds them; a qualified use of one names the
    // rule (PRIVATE_DECLARATION) instead of reporting an unknown member.
    std::unordered_set<std::string> private_names;
};

// The contextual constants visible where the frontend renames an
// expression: one map per enclosing block, innermost last, from a name to
// the declaration's (renamed) initializer. Uses become LiteralConstantExpr.
thread_local std::vector<std::unordered_map<std::string, const Expr*>> literal_constant_scopes;
struct LiteralConstantScope {
    LiteralConstantScope() { literal_constant_scopes.emplace_back(); }
    ~LiteralConstantScope() { literal_constant_scopes.pop_back(); }
    LiteralConstantScope(const LiteralConstantScope&) = delete;
    LiteralConstantScope& operator=(const LiteralConstantScope&) = delete;
};
const Expr* visible_literal_constant(const std::string& name) {
    for (auto scope = literal_constant_scopes.rbegin(); scope != literal_constant_scopes.rend();
         ++scope) {
        if (const auto found = scope->find(name); found != scope->end()) return found->second;
    }
    return nullptr;
}

[[noreturn]] void literal_constant_error(SourceSpan span) {
    frontend_error("LITERAL_CONSTANT",
                   "A literal constant has type real or com and a constant exact initializer.",
                   span);
}

bool exact_intrinsic(std::string_view name) {
    return name == "$std.real.atom" || name == "$std.real.unary" || name == "$std.real.op";
}

// A copy of a contextual constant's initializer for one use: literals,
// arithmetic, numeric conversions of them, the exact intrinsics with their
// literal provider and opcode, and other contextual constants. Anything
// else is not a constant exact initializer.
ExprPtr clone_literal_initializer(const Expr& source, SourceSpan use_span) {
    auto result = std::make_unique<Expr>();
    result->span = use_span;
    if (const auto* value = std::get_if<IntegerExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<RealLiteralExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<StringExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&source.data); unary && unary->op == "-") {
        result->data = UnaryExpr{unary->op, clone_literal_initializer(*unary->operand, use_span)};
        return result;
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&source.data);
        binary && (binary->op == "+" || binary->op == "-" || binary->op == "*" ||
                   binary->op == "/" || binary->op == "^")) {
        result->data = BinaryExpr{binary->op, clone_literal_initializer(*binary->left, use_span),
                                  clone_literal_initializer(*binary->right, use_span)};
        return result;
    }
    if (const auto* constant = std::get_if<LiteralConstantExpr>(&source.data)) {
        result->data = LiteralConstantExpr{constant->name,
                                           clone_literal_initializer(*constant->value, use_span),
                                           constant->origin};
        return result;
    }
    const auto clone_args = [&](const std::vector<CallArg>& args) {
        std::vector<CallArg> out;
        out.reserve(args.size());
        for (const auto& argument : args) {
            if (argument.writable) literal_constant_error(source.span);
            out.push_back(CallArg{argument.name, false,
                                  clone_literal_initializer(*argument.value, use_span), use_span});
        }
        return out;
    };
    if (const auto* call = std::get_if<CallExpr>(&source.data);
        call && call->type_arguments.empty() &&
        (exact_intrinsic(call->callee) ||
         (numeric_type_name(call->callee) != nullptr && call->args.size() == 1))) {
        result->data = CallExpr{call->callee, clone_args(call->args), {}};
        return result;
    }
    if (const auto* call = std::get_if<MethodCallExpr>(&source.data)) {
        const auto* receiver = std::get_if<NameExpr>(&call->receiver->data);
        if (receiver && !receiver->this_qualifier && receiver->name == "real" &&
            (call->method == "atom" || call->method == "unary" || call->method == "op") &&
            call->type_arguments.empty()) {
            auto cloned_receiver = std::make_unique<Expr>();
            cloned_receiver->span = use_span;
            cloned_receiver->data = NameExpr{receiver->name};
            result->data = MethodCallExpr{std::move(cloned_receiver), call->method,
                                          clone_args(call->args), {}};
            return result;
        }
    }
    literal_constant_error(source.span);
}

// A use of a contextual constant: the name as written and a fresh copy of
// its initializer.
ExprPtr literal_constant_use(std::string name, const Expr& initializer, SourceSpan use_span) {
    auto result = std::make_unique<Expr>();
    result->span = use_span;
    result->data = LiteralConstantExpr{std::move(name),
                                       clone_literal_initializer(initializer, use_span),
                                       &initializer};
    return result;
}

// A qualified use of a private declaration of another module.
[[noreturn]] void private_declaration_error(
    const std::string& name, const std::string& module, SourceSpan span) {
    frontend_error("PRIVATE_DECLARATION",
                   "'" + name + "' is private to module '" + module + "'.", span);
}

ExprPtr clone_package_constant_expression(const Expr& source, SourceSpan use_span) {
    auto result = std::make_unique<Expr>();
    result->span = use_span;
    result->contextual_default_type = source.contextual_default_type;
    if (const auto* value = std::get_if<IntegerExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<RealLiteralExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<StringExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<BoolExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (std::holds_alternative<NoneExpr>(source.data)) {
        result->data = NoneExpr{};
        return result;
    }
    if (const auto* constant = std::get_if<LiteralConstantExpr>(&source.data)) {
        result->data = LiteralConstantExpr{constant->name,
                                           clone_literal_initializer(*constant->value, use_span),
                                           constant->origin};
        return result;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&source.data)) {
        result->data = UnaryExpr{
            unary->op,
            clone_package_constant_expression(*unary->operand, use_span)};
        return result;
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&source.data)) {
        result->data = BinaryExpr{
            binary->op,
            clone_package_constant_expression(*binary->left, use_span),
            clone_package_constant_expression(*binary->right, use_span),
            use_span};
        return result;
    }
    if (const auto* chain = std::get_if<ComparisonChainExpr>(&source.data)) {
        ComparisonChainExpr copy;
        for (const auto& operand : chain->operands)
            copy.operands.push_back(clone_package_constant_expression(*operand, use_span));
        copy.ops = chain->ops;
        copy.op_spans.assign(chain->op_spans.size(), use_span);
        result->data = std::move(copy);
        return result;
    }
    if (const auto* call = std::get_if<CallExpr>(&source.data)) {
        const bool scalar_numeric_cast = numeric_type_name(call->callee) != nullptr;
        if (scalar_numeric_cast && call->type_arguments.empty() &&
            call->args.size() == 1 && !call->args[0].writable) {
            std::vector<CallArg> args;
            args.push_back(CallArg{
                call->args[0].name,
                false,
                clone_package_constant_expression(*call->args[0].value, use_span),
                use_span});
            result->data = CallExpr{call->callee, std::move(args), {}};
            return result;
        }
    }
    if (const auto* call = std::get_if<MethodCallExpr>(&source.data)) {
        const auto* receiver = std::get_if<NameExpr>(&call->receiver->data);
        if (receiver && !receiver->this_qualifier && receiver->name == "real" &&
            call->method == "atom" &&
            call->type_arguments.empty() && call->args.size() == 2 &&
            !call->args[0].writable && !call->args[1].writable) {
            auto cloned_receiver = std::make_unique<Expr>();
            cloned_receiver->span = use_span;
            cloned_receiver->data = NameExpr{receiver->name};
            std::vector<CallArg> args;
            args.reserve(2);
            for (const auto& argument : call->args) {
                args.push_back(CallArg{
                    argument.name,
                    false,
                    clone_package_constant_expression(*argument.value, use_span),
                    use_span});
            }
            result->data = MethodCallExpr{
                std::move(cloned_receiver), "atom", std::move(args), {}};
            return result;
        }
    }
    frontend_error(
        "PACKAGE_CONST",
        "Imported top-level const initializers must be side-effect-free scalar constant expressions.",
        source.span);
}

void insert_class_export(
    Exports& exports,
    const std::string& name,
    const std::string& target,
    SourceSpan span) {
    const auto dot = name.find('.');
    if (dot == std::string::npos) {
        if (exports.namespaces.contains(name) ||
            !exports.classes.emplace(name, target).second) {
            frontend_error(
                "DUPLICATE_NAME",
                "Class export '" + name + "' conflicts with another exported declaration.",
                span);
        }
        return;
    }

    const auto head = name.substr(0, dot);
    const auto tail = name.substr(dot + 1);
    if (exports.classes.contains(head) || exports.enums.contains(head) ||
        exports.functions.contains(head) || exports.values.contains(head) ||
        exports.constants.contains(head)) {
        frontend_error(
            "DUPLICATE_NAME",
            "Namespace '" + head + "' conflicts with another exported declaration.",
            span);
    }
    auto& nested = exports.namespaces[head];
    if (!nested) nested = std::make_shared<Exports>();
    insert_class_export(*nested, tail, target, span);
}

// Follows `a.b.c` through re-exported namespaces. Returns the innermost
// namespace and leaves the unresolved tail (a declaration name, possibly with
// an enum variant) in `rest`.
const Exports* descend_namespaces(const Exports& root, const std::string& path, std::string& rest) {
    const Exports* current = &root;
    std::string remaining = path;
    while (true) {
        const auto dot = remaining.find('.');
        const auto head = dot == std::string::npos ? remaining : remaining.substr(0, dot);
        const auto next = current->namespaces.find(head);
        if (next == current->namespaces.end() || dot == std::string::npos) break;
        current = next->second.get();
        remaining = remaining.substr(dot + 1);
    }
    rest = remaining;
    return current;
}

Exports standard_exports(const std::string& module, SourceSpan span) {
    if (!is_standard_module(module)) {
        frontend_error("UNKNOWN_STANDARD_MODULE",
                       "Unknown standard namespace '" + module +
                           "'. Standard namespaces come from the language registry; unquoted non-standard imports resolve installed packages.",
                       span);
    }
    Exports exports;
    if (module == "file") {
        exports.classes.emplace("Handle", standard_class::file_handle);
        for (const char* name : {"open", "create", "append", "read", "write", "read_bin", "write_bin", "exists", "is_directory", "remove", "copy", "move", "mkdir", "list"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "environment") {
        for (const char* name : {"get", "has"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "test") {
        for (const char* name : {"check", "equal"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "time") {
        exports.classes.emplace("Instant", standard_class::time_instant);
        exports.classes.emplace("Duration", standard_class::time_duration);
        for (const char* name : {"now", "since", "seconds", "sleep"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "gpu") {
        exports.functions.emplace("sync", std::string(*standard_function_target(module, "sync")));
    } else if (module == "autograd") {
        exports.classes.emplace("Target", standard_class::autograd_target);
        exports.functions.emplace("target", std::string(*standard_function_target(module, "target")));
    } else if (module == "reflect") {
        exports.pseudo_types.emplace("Each","$std.reflect.Each");
        for (const char* name : {"collect", "paths", "type_name", "each", "each_paths"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "random") {
        exports.classes.emplace("Generator", standard_class::random_generator);
        exports.functions.emplace("generator", std::string(*standard_function_target(module, "generator")));
    } else if (module == "process") {
        exports.classes.emplace("Result", standard_class::process_result);
        for (const char* name : {"run", "shell", "exit"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "map") {
        exports.classes.emplace("Map", standard_class::map);
    } else if (module == "set") {
        exports.classes.emplace("Set", standard_class::set);
    } else if (module == "json") {
        exports.classes.emplace("Value", standard_class::json_value);
        exports.functions.emplace("parse", std::string(*standard_function_target(module, "parse")));
    } else if (module == "http") {
        exports.classes.emplace("Response", standard_class::http_response);
        exports.functions.emplace("get", std::string(*standard_function_target(module, "get")));
    } else if (module == "tensor") {
        exports.functions.emplace("zeros", std::string(*standard_function_target(module, "zeros")));
        exports.functions.emplace("ones", std::string(*standard_function_target(module, "ones")));
    }
    return exports;
}


SourceSpan standard_span() {
    SourceSpan span{};
    span.start.offset = std::numeric_limits<std::size_t>::max();
    span.end.offset = std::numeric_limits<std::size_t>::max();
    return span;
}

TypeName standard_type(std::string name) {
    TypeName type;
    type.name = std::move(name);
    type.span = standard_span();
    return type;
}

ExprPtr standard_name(std::string name) {
    auto expression = std::make_unique<Expr>();
    expression->span = standard_span();
    expression->data = NameExpr{std::move(name)};
    return expression;
}

// `this.NAME` in a compiler-built member body.
ExprPtr standard_this_field(std::string name) {
    auto expression = standard_name(std::move(name));
    std::get<NameExpr>(expression->data).this_qualifier = standard_span();
    return expression;
}

CallArg standard_arg(ExprPtr value) {
    return CallArg{std::nullopt, false, std::move(value), standard_span()};
}

ExprPtr standard_call(std::string callee, std::vector<CallArg> args = {}) {
    auto expression = std::make_unique<Expr>();
    expression->span = standard_span();
    expression->data = CallExpr{std::move(callee), std::move(args), {}};
    return expression;
}

StmtPtr standard_return(ExprPtr value) {
    auto statement = std::make_unique<Stmt>();
    statement->span = standard_span();
    statement->data = ReturnStmt{std::move(value)};
    return statement;
}

Parameter standard_parameter(std::string name, std::string type) {
    Parameter parameter;
    parameter.name = std::move(name);
    parameter.type = standard_type(std::move(type));
    parameter.span = standard_span();
    return parameter;
}

TypeName standard_union_type(std::initializer_list<std::string> cases) {
    TypeName type;
    type.name = "union";
    type.span = standard_span();
    for (const auto& item : cases) type.arguments.push_back(standard_type(item));
    return type;
}

FunctionDecl standard_method(std::string name, std::vector<Parameter> parameters,
                             TypeName result, ExprPtr value) {
    FunctionDecl method;
    method.name = std::move(name);
    method.parameters = std::move(parameters);
    method.return_type = std::move(result);
    method.body.push_back(standard_return(std::move(value)));
    method.span = standard_span();
    return method;
}

FunctionDecl standard_method(std::string name, std::vector<Parameter> parameters,
                             std::string result, ExprPtr value) {
    return standard_method(std::move(name), std::move(parameters),
                           standard_type(std::move(result)), std::move(value));
}

FieldDecl standard_field(std::string name, std::string type) {
    FieldDecl field;
    field.name = std::move(name);
    field.type = standard_type(std::move(type));
    field.span = standard_span();
    return field;
}

std::vector<ClassDecl> standard_declarations(const std::string& module) {
    std::vector<ClassDecl> declarations;
    if (module == "autograd") {
        ClassDecl target;
        target.name = standard_class::autograd_target;
        target.span = standard_span();
        target.fields.push_back(standard_field("$handle", "nat64"));
        declarations.push_back(std::move(target));
    } else if (module == "file") {
        ClassDecl handle;
        handle.name = standard_class::file_handle;
        handle.span = standard_span();
        handle.fields.push_back(standard_field("$handle", "nat64"));
        declarations.push_back(std::move(handle));
    } else if (module == "time") {
        ClassDecl instant;
        instant.name = standard_class::time_instant;
        instant.span = standard_span();
        instant.fields.push_back(standard_field("$seconds", "real64"));
        declarations.push_back(std::move(instant));

        ClassDecl duration;
        duration.name = standard_class::time_duration;
        duration.span = standard_span();
        duration.fields.push_back(standard_field("$seconds", "real64"));
        duration.methods.push_back(standard_method(
            "seconds", std::vector<Parameter>{}, "real64", standard_this_field("$seconds")));
        declarations.push_back(std::move(duration));
    } else if (module == "random") {
        ClassDecl generator;
        generator.name = standard_class::random_generator;
        generator.span = standard_span();
        generator.fields.push_back(standard_field("$state", "nat64"));

        std::vector<Parameter> int_parameters;
        int_parameters.push_back(standard_parameter("start", "int"));
        int_parameters.push_back(standard_parameter("end", "int"));
        std::vector<CallArg> int_arguments;
        int_arguments.push_back(standard_arg(standard_name("start")));
        int_arguments.push_back(standard_arg(standard_name("end")));
        generator.methods.push_back(standard_method(
            "int", std::move(int_parameters), "int",
            standard_call("$std.random.int", std::move(int_arguments))));

        generator.methods.push_back(standard_method(
            "real64", std::vector<Parameter>{}, "real64", standard_call("$std.random.real64")));
        generator.methods.push_back(standard_method(
            "bool", std::vector<Parameter>{}, "bool", standard_call("$std.random.bool")));
        // Each draw advances the generator's state: `.generator.int(1, 6)`.
        for (auto& method : generator.methods) method.mutation_marker = true;
        declarations.push_back(std::move(generator));
    } else if (module == "process") {
        ClassDecl result;
        result.name = standard_class::process_result;
        result.span = standard_span();
        result.fields.push_back(standard_field("status", "int"));
        result.fields.push_back(standard_field("output", "string"));
        result.fields.push_back(standard_field("error", "string"));
        result.fields.push_back(standard_field("started", "bool"));
        declarations.push_back(std::move(result));
    } else if (module == "map") {
        constexpr std::string_view source = R"QUI(class Map<K, V>()
    K[] __keys = []
    V[] __values = []
    int64[] __hashes = []
    bool[] __active = []
    int64[] __slots = array(8, fill = -1)
    int64 __size = 0
    int64 __tombstones = 0
    int64 __empty_slot = -1
    int64 __last_index = -1
    int64 __last_hash = -1
    int64 __last_slot = -1
    K[] __last_key = []
    int64 __version = 0
    int64 __last_version = -1

    int64 __hash(K key)
        int[] __encoded = key.string().codepoints()
        int64 __result = 0
        for auto __unit = __encoded[]
            int64 __unit64 = int64(__unit)
            __result = (__result * 131 + __unit64) % 2147483647
        return __result

    int64 __slot_of(K key, int64 hash)
        int64 __capacity = int64(len(__slots))
        int64 __mask = __capacity - 1
        int64 __slot = hash AND __mask
        int64 __scanned = 0
        int64 __first_tombstone = -1
        while __scanned < __capacity
            int64 __index = __slots[__slot]
            if __index == -1
                if __first_tombstone >= 0
                    __empty_slot = __first_tombstone
                else
                    __empty_slot = __slot
                return -1
            if __index == -2 and __first_tombstone < 0
                __first_tombstone = __slot
            if __index >= 0 and __hashes[__index] == hash and __keys[__index] == key
                return __slot
            __slot = (__slot + 1) AND __mask
            __scanned += 1
        __empty_slot = __first_tombstone
        return -1

    int64 __find(K key, int64 hash)
        int64 __slot = __slot_of(key, hash)
        if __slot < 0
            return -1
        return __slots[__slot]

    void __place(int64 index)
        int64 __capacity = int64(len(__slots))
        int64 __mask = __capacity - 1
        int64 __slot = __hashes[index] AND __mask
        while __slots[__slot] >= 0
            __slot = (__slot + 1) AND __mask
        __slots[__slot] = index

    void __rehash(int64 capacity)
        __slots = array(nat(capacity), fill = -1)
        for 0 <= nat __index < len(__keys)
            if __active[__index]
                __place(int64(__index))
        __tombstones = 0
        return void

    void __compact()
        K[] __new_keys = array(nat(__size))
        V[] __new_values = array(nat(__size))
        int64[] __new_hashes = array(nat(__size), fill = 0)
        bool[] __new_active = array(nat(__size), fill = false)
        int64 __write = 0
        for 0 <= nat __index < len(__keys)
            if __active[__index]
                __new_keys[__write] = __keys[__index]
                __new_values[__write] = __values[__index]
                __new_hashes[__write] = __hashes[__index]
                __new_active[__write] = true
                __write += 1
        __keys = __new_keys
        __values = __new_values
        __hashes = __new_hashes
        __active = __new_active
        __rehash(int64(len(__slots)))
        __version += 1
        __last_version = -1
        return void

    bool has(K key)
        int64 __hash_value = __hash(key)
        return __find(key, __hash_value) >= 0

    V | none get(K key)
        int64 __hash_value = __hash(key)
        int64 __index = __find(key, __hash_value)
        __last_hash = __hash_value
        __last_index = __index
        __last_slot = __empty_slot
        __last_version = __version
        if __index >= 0
            return __values[__index]
        if len(__last_key) == 0
            __last_key = __last_key.append(key)
        else
            __last_key[0] = key
        return none

    void .set(K key, V value)
        int64 __hash_value = -1
        if __last_version == __version and __last_index >= 0
            int64 __cached = __last_index
            if __keys[__cached] == key
                __values[__cached] = value
                return void

        bool __reuse_missing = false
        if __last_version == __version and __last_index < 0 and len(__last_key) == 1
            if __last_key[0] == key
                __reuse_missing = true
                __hash_value = __last_hash

        int64 __index = -1
        int64 __insert_slot = -1
        if __reuse_missing
            __insert_slot = __last_slot
        else
            __hash_value = __hash(key)
            __index = __find(key, __hash_value)
            __insert_slot = __empty_slot

        if __index >= 0
            __values[__index] = value
            __last_hash = __hash_value
            __last_index = __index
            __last_version = __version
            return void

        int64 __new_index = int64(len(__keys))
        __keys = __keys.append(key)
        __values = __values.append(value)
        __hashes = __hashes.append(__hash_value)
        __active = __active.append(true)
        __size += 1

        int64 __slot_count = int64(len(__slots))
        if __size > __slot_count - __slot_count / 4
            __rehash(__slot_count * 2)
        else
            if __slots[__insert_slot] == -2
                __tombstones -= 1
            __slots[__insert_slot] = __new_index
        __version += 1
        __last_version = -1
        return void

    bool .remove(K key)
        int64 __hash_value = __hash(key)
        int64 __slot = __slot_of(key, __hash_value)
        if __slot < 0
            return false

        int64 __removed_index = __slots[__slot]
        __active[__removed_index] = false
        __slots[__slot] = -2
        __size -= 1
        __tombstones += 1
        __version += 1
        __last_version = -1
        int64 __key_count = int64(len(__keys))
        int64 __slot_count = int64(len(__slots))
        if __key_count > 64 and __size * 2 <= __key_count
            __compact()
        elif __tombstones > __slot_count / 4
            __rehash(__slot_count)
        return true

    nat size()
        return nat(__size)

    K[] keys()
        int64 __count = int64(len(__keys))
        if __size != __count
            __compact()
        return __keys

    V[] values()
        int64 __count = int64(len(__values))
        if __size != __count
            __compact()
        return __values
)QUI";
        Parser parser(Lexer(std::string(source)).scan(), 20);
        auto program = parser.parse();
        if (program.classes.size() != 1) {
            throw std::logic_error("invalid built-in map declaration");
        }
        auto map = std::move(program.classes.front());
        map.name = standard_class::map;
        map.methods.front().return_type.name = map.name;
        map.span = standard_span();
        declarations.push_back(std::move(map));
    } else if (module == "http") {
        ClassDecl response;
        response.name = standard_class::http_response;
        response.span = standard_span();
        response.fields.push_back(standard_field("status", "int"));
        response.fields.push_back(standard_field("body", "bin"));
        response.fields.push_back(standard_field("$headers", "nat64"));

        std::vector<Parameter> header_parameters;
        header_parameters.push_back(standard_parameter("name", "string"));
        std::vector<CallArg> header_arguments;
        header_arguments.push_back(standard_arg(standard_name("name")));
        response.methods.push_back(standard_method(
            "header", std::move(header_parameters),
            standard_union_type({"string", "none"}),
            standard_call("$std.http.header", std::move(header_arguments))));
        declarations.push_back(std::move(response));
    } else if (module == "json") {
        ClassDecl value;
        value.name = standard_class::json_value;
        value.span = standard_span();
        value.fields.push_back(standard_field("$handle", "nat64"));

        value.methods.push_back(standard_method(
            "kind", std::vector<Parameter>{}, "string", standard_call("$std.json.kind")));
        value.methods.push_back(standard_method(
            "size", std::vector<Parameter>{},
            standard_union_type({"nat", "error"}), standard_call("$std.json.size")));

        std::vector<Parameter> get_parameters;
        get_parameters.push_back(standard_parameter("key", "string"));
        std::vector<CallArg> get_arguments;
        get_arguments.push_back(standard_arg(standard_name("key")));
        value.methods.push_back(standard_method(
            "get", std::move(get_parameters),
            standard_union_type({standard_class::json_value, "none", "error"}),
            standard_call("$std.json.get", std::move(get_arguments))));

        std::vector<Parameter> at_parameters;
        at_parameters.push_back(standard_parameter("index", "nat"));
        std::vector<CallArg> at_arguments;
        at_arguments.push_back(standard_arg(standard_name("index")));
        value.methods.push_back(standard_method(
            "at", std::move(at_parameters),
            standard_union_type({standard_class::json_value, "none", "error"}),
            standard_call("$std.json.at", std::move(at_arguments))));

        value.methods.push_back(standard_method(
            "text", std::vector<Parameter>{},
            standard_union_type({"string", "error"}), standard_call("$std.json.text")));
        value.methods.push_back(standard_method(
            "integer", std::vector<Parameter>{},
            standard_union_type({"int", "error"}), standard_call("$std.json.integer")));
        value.methods.push_back(standard_method(
            "number", std::vector<Parameter>{},
            standard_union_type({"real64", "error"}), standard_call("$std.json.number")));
        value.methods.push_back(standard_method(
            "real", std::vector<Parameter>{},
            standard_union_type({"real", "error"}), standard_call("$std.json.real")));
        value.methods.push_back(standard_method(
            "boolean", std::vector<Parameter>{},
            standard_union_type({"bool", "error"}), standard_call("$std.json.boolean")));
        value.methods.push_back(standard_method(
            "encode", std::vector<Parameter>{}, "string", standard_call("$std.json.encode")));

        std::vector<Parameter> equal_parameters;
        equal_parameters.push_back(standard_parameter("other", standard_class::json_value));
        std::vector<CallArg> equal_arguments;
        equal_arguments.push_back(standard_arg(standard_name("other")));
        value.methods.push_back(standard_method(
            "equal", std::move(equal_parameters), "bool",
            standard_call("$std.json.equal", std::move(equal_arguments))));
        declarations.push_back(std::move(value));
    } else if (module == "set") {
        constexpr std::string_view source = R"QUI(class Set<T>()
    T[] __values = []
    int64[] __hashes = []
    bool[] __active = []
    int64[] __slots = array(8, fill = -1)
    int64 __size = 0
    int64 __tombstones = 0
    int64 __empty_slot = -1

    int64 __hash(T value)
        int[] __encoded = value.string().codepoints()
        int64 __result = 0
        for auto __unit = __encoded[]
            int64 __unit64 = int64(__unit)
            __result = (__result * 131 + __unit64) % 2147483647
        return __result

    int64 __slot_of(T value, int64 hash)
        int64 __capacity = int64(len(__slots))
        int64 __mask = __capacity - 1
        int64 __slot = hash AND __mask
        int64 __scanned = 0
        int64 __first_tombstone = -1
        while __scanned < __capacity
            int64 __index = __slots[__slot]
            if __index == -1
                if __first_tombstone >= 0
                    __empty_slot = __first_tombstone
                else
                    __empty_slot = __slot
                return -1
            if __index == -2 and __first_tombstone < 0
                __first_tombstone = __slot
            if __index >= 0 and __hashes[__index] == hash and __values[__index] == value
                return __slot
            __slot = (__slot + 1) AND __mask
            __scanned += 1
        __empty_slot = __first_tombstone
        return -1

    int64 __find(T value, int64 hash)
        int64 __slot = __slot_of(value, hash)
        if __slot < 0
            return -1
        return __slots[__slot]

    void __place(int64 index)
        int64 __capacity = int64(len(__slots))
        int64 __mask = __capacity - 1
        int64 __slot = __hashes[index] AND __mask
        while __slots[__slot] >= 0
            __slot = (__slot + 1) AND __mask
        __slots[__slot] = index

    void __rehash(int64 capacity)
        __slots = array(nat(capacity), fill = -1)
        for 0 <= nat __index < len(__values)
            if __active[__index]
                __place(int64(__index))
        __tombstones = 0
        return void

    void __compact()
        T[] __new_values = array(nat(__size))
        int64[] __new_hashes = array(nat(__size), fill = 0)
        bool[] __new_active = array(nat(__size), fill = false)
        int64 __write = 0
        for 0 <= nat __index < len(__values)
            if __active[__index]
                __new_values[__write] = __values[__index]
                __new_hashes[__write] = __hashes[__index]
                __new_active[__write] = true
                __write += 1
        __values = __new_values
        __hashes = __new_hashes
        __active = __new_active
        __rehash(int64(len(__slots)))
        return void

    bool has(T value)
        int64 __hash_value = __hash(value)
        return __find(value, __hash_value) >= 0

    void .add(T value)
        int64 __hash_value = __hash(value)
        if __find(value, __hash_value) >= 0
            return void

        int64 __new_index = int64(len(__values))
        int64 __insert_slot = __empty_slot
        __values = __values.append(value)
        __hashes = __hashes.append(__hash_value)
        __active = __active.append(true)
        __size += 1

        int64 __slot_count = int64(len(__slots))
        if __size > __slot_count - __slot_count / 4
            __rehash(__slot_count * 2)
        else
            if __slots[__insert_slot] == -2
                __tombstones -= 1
            __slots[__insert_slot] = __new_index
        return void

    bool .remove(T value)
        int64 __hash_value = __hash(value)
        int64 __slot = __slot_of(value, __hash_value)
        if __slot < 0
            return false

        int64 __removed_index = __slots[__slot]
        __active[__removed_index] = false
        __slots[__slot] = -2
        __size -= 1
        __tombstones += 1
        int64 __value_count = int64(len(__values))
        int64 __slot_count = int64(len(__slots))
        if __value_count > 64 and __size * 2 <= __value_count
            __compact()
        elif __tombstones > __slot_count / 4
            __rehash(__slot_count)
        return true

    nat size()
        return nat(__size)

    T[] values()
        int64 __count = int64(len(__values))
        if __size != __count
            __compact()
        return __values
)QUI";
        Parser parser(Lexer(std::string(source)).scan(), 20);
        auto program = parser.parse();
        if (program.classes.size() != 1) {
            throw std::logic_error("invalid built-in set declaration");
        }
        auto set = std::move(program.classes.front());
        set.name = standard_class::set;
        set.methods.front().return_type.name = set.name;
        set.span = standard_span();
        declarations.push_back(std::move(set));
    }
    for (auto& declaration : declarations) declaration.standard_library = true;
    return declarations;
}

struct ImportBinding {
    std::string ns;
    Exports exports;
};

void rename_type(
    TypeName& type,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& type_parameters) {
    // A bare axis name refers only to a nat value, and a shape variable is a
    // new name: neither can be a type parameter of the declaration.
    for (const auto& axis : type.shape_axes) {
        if (!type_parameters.contains(axis.name)) continue;
        if (axis.kind == ShapeAxisKind::Bare) {
            frontend_error("SHAPE_VARIABLE",
                           "'" + axis.name +
                               "' is a type parameter, not a nat value; choose another axis name.",
                           axis.span);
        }
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual) {
            frontend_error("SHADOWING",
                           "'" + axis.name + "' already names a type parameter; 'nat " + axis.name +
                               "' would declare a new shape variable. Choose another name.",
                           axis.span);
        }
    }
    if (type.name == "union" || type.name == "$tuple" || type.name == "$std.reflect.Each") {
        for (auto& argument : type.arguments) {
            rename_type(argument, ns, local_classes, imports, type_parameters);
        }
        return;
    }

    for (auto& argument : type.arguments) {
        rename_type(argument, ns, local_classes, imports, type_parameters);
    }
    for (auto& parameter : type.function_parameters) {
        rename_type(parameter, ns, local_classes, imports, type_parameters);
    }

    if (is_language_type_name(type.name) || type_parameters.contains(type.name)) return;

    if (type.name.find('.') != std::string::npos) {
        if (local_classes.contains(type.name)) {
            type.name = qualify(ns, type.name);
            return;
        }
        const auto head = first_segment(type.name);
        if (const auto it = imports.find(head); it != imports.end()) {
            const auto full_suffix = suffix_after_first(type.name);
            if (full_suffix.empty()) {
                frontend_error("UNKNOWN_TYPE", "Imported module alias cannot be used as a type.", type.span);
            }
            std::string suffix;
            const Exports& module = *descend_namespaces(it->second.exports, full_suffix, suffix);
            if(const auto pseudo=module.pseudo_types.find(suffix);pseudo!=module.pseudo_types.end()) {type.name=pseudo->second;return;}
            if (const auto cls = module.classes.find(suffix);
                cls != module.classes.end()) {
                type.name = cls->second;
                return;
            }
            if (const auto en = module.enums.find(suffix);
                en != module.enums.end()) {
                type.name = en->second;
                return;
            }
            const auto dot = suffix.find('.');
            if (dot != std::string::npos) {
                const auto enum_name = suffix.substr(0, dot);
                if (const auto en = module.enums.find(enum_name);
                    en != module.enums.end()) {
                    type.name = en->second + suffix.substr(dot);
                    return;
                }
            }
            if (module.private_names.contains(suffix) ||
                module.private_names.contains(suffix.substr(0, suffix.find('.')))) {
                const auto shown = type.name.substr(0, type.name.size() - suffix.size() - 1);
                private_declaration_error(suffix, shown, type.span);
            }
            frontend_error("UNKNOWN_TYPE",
                           "Module '" + head + "' has no exported type '" + full_suffix + "'.",
                           type.span);
        }
        const auto dot = type.name.find('.');
        const auto head_name = dot == std::string::npos ? type.name : type.name.substr(0, dot);
        if (local_classes.contains(head_name)) type.name = qualify(ns, type.name);
        return;
    }

    if (local_classes.contains(type.name)) {
        type.name = qualify(ns, type.name);
    }
}

// The namespace an expression such as `dnn` or `dnn.mode` names, if any:
// an import alias followed by re-exported aliases.
std::optional<std::string> expression_qualified_name(const Expr& expression) {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        // `this.NAME` is a receiver field, never a namespace or a class.
        if (name->this_qualifier || name->self_reference) return std::nullopt;
        return name->name;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        auto base = expression_qualified_name(*member->base);
        if (!base) return std::nullopt;
        return *base + "." + member->name;
    }
    return std::nullopt;
}

const Exports* expression_namespace(
    const Expr& expression,
    const std::unordered_map<std::string, ImportBinding>& imports,
    std::string& spelling) {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (name->this_qualifier || name->self_reference) return nullptr;
        const auto import = imports.find(name->name);
        if (import == imports.end()) return nullptr;
        spelling = name->name;
        return &import->second.exports;
    }
    if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
        const auto* base = expression_namespace(*member->base, imports, spelling);
        if (!base) return nullptr;
        const auto nested = base->namespaces.find(member->name);
        if (nested == base->namespaces.end()) return nullptr;
        spelling += "." + member->name;
        return nested->second.get();
    }
    return nullptr;
}

void rename_expr(
    Expr& expression,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_set<std::string>& local_functions,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& type_parameters);

void rename_call_args(
    std::vector<CallArg>& args,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_set<std::string>& local_functions,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& type_parameters) {
    for (auto& arg : args) {
        rename_expr(*arg.value, ns, local_classes, local_functions, imports, type_parameters);
    }
}

void rename_expr(
    Expr& expression,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_set<std::string>& local_functions,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& type_parameters) {
    if (auto* node = std::get_if<NameExpr>(&expression.data)) {
        if (!node->this_qualifier && !node->self_reference) {
            if (const auto* initializer = visible_literal_constant(node->name)) {
                auto use = literal_constant_use(node->name, *initializer, expression.span);
                expression.data = std::move(use->data);
            }
        }
        return;
    }
    if (auto* node = std::get_if<LiteralConstantExpr>(&expression.data)) {
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (auto& item : node->expressions) {
            rename_expr(*item, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<TupleExpr>(&expression.data)) {
        for (auto& item : node->elements) {
            rename_expr(*item, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
        if (node->type) rename_type(*node->type, ns, local_classes, imports, type_parameters);
        visit_array_comprehension(*node, [&](Expr& child) {
            rename_expr(child, ns, local_classes, local_functions, imports, type_parameters);
        });
        return;
    }
    if (auto* node = std::get_if<ArrayExpr>(&expression.data)) {
        for (auto& item : node->elements) {
            rename_expr(*item, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<IndexExpr>(&expression.data)) {
        rename_expr(*node->base, ns, local_classes, local_functions, imports, type_parameters);
        for (auto& item : node->items) {
            if (item.index) rename_expr(*item.index, ns, local_classes, local_functions, imports, type_parameters);
            if (item.start) rename_expr(*item.start, ns, local_classes, local_functions, imports, type_parameters);
            if (item.stop) rename_expr(*item.stop, ns, local_classes, local_functions, imports, type_parameters);
            if (item.step) rename_expr(*item.step, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<MemberExpr>(&expression.data)) {
        std::string module_spelling;
        if (const auto* module = expression_namespace(*node->base, imports, module_spelling)) {
            if (const auto constant = module->constants.find(node->name);
                constant != module->constants.end()) {
                auto replacement =
                    clone_package_constant_expression(*constant->second, expression.span);
                // The name as the use writes it, read before the use is replaced.
                const auto qualified = module_spelling + "." + node->name;
                if (auto* use = std::get_if<LiteralConstantExpr>(&replacement->data))
                    use->name = qualified;
                expression.data = std::move(replacement->data);
                expression.contextual_default_type =
                    std::move(replacement->contextual_default_type);
                if (expression.contextual_default_type) expression.constant_name = qualified;
                rename_expr(
                    expression, ns, local_classes, local_functions, imports,
                    type_parameters);
                return;
            }
            if (const auto value = module->values.find(node->name);
                value != module->values.end()) {
                expression.data = NameExpr{value->second};
                return;
            }
            if (const auto function = module->functions.find(node->name);
                function != module->functions.end()) {
                expression.data = NameExpr{function->second};
                return;
            }
            if (const auto en = module->enums.find(node->name);
                en != module->enums.end()) {
                expression.data = NameExpr{en->second};
                return;
            }
            if (module->namespaces.contains(node->name)) {
                frontend_error("NAMESPACE_VALUE",
                               "'" + module_spelling + "." + node->name +
                                   "' is a namespace, not a value.",
                               expression.span);
            }
            if (module->private_names.contains(node->name))
                private_declaration_error(node->name, module_spelling, expression.span);
            frontend_error("UNKNOWN_MODULE_MEMBER",
                           "Module '" + module_spelling + "' has no exported value '" +
                               node->name + "'.",
                           expression.span);
        }
        if (auto* base = std::get_if<NameExpr>(&node->base->data)) {
            if (!base->this_qualifier && !base->self_reference && local_classes.contains(base->name))
                base->name = qualify(ns, base->name);
        }
        rename_expr(*node->base, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<UnaryExpr>(&expression.data)) {
        rename_expr(*node->operand, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<BinaryExpr>(&expression.data)) {
        rename_expr(*node->left, ns, local_classes, local_functions, imports, type_parameters);
        rename_expr(*node->right, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
        for (auto& operand : node->operands)
            rename_expr(*operand, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<TryExpr>(&expression.data)) {
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<IfExpr>(&expression.data)) {
        for (auto& condition : node->conditions)
            rename_expr(*condition, ns, local_classes, local_functions, imports, type_parameters);
        for (auto& value : node->values)
            rename_expr(*value, ns, local_classes, local_functions, imports, type_parameters);
        rename_expr(*node->otherwise, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<CallExpr>(&expression.data)) {
        for (auto& type_argument : node->type_arguments) {
            rename_type(type_argument, ns, local_classes, imports, type_parameters);
        }
        rename_call_args(node->args, ns, local_classes, local_functions, imports, type_parameters);
        if (local_classes.contains(node->callee) || local_functions.contains(node->callee)) {
            node->callee = qualify(ns, node->callee);
        }
        return;
    }
    if (auto* node = std::get_if<MethodCallExpr>(&expression.data)) {
        auto* receiver_name = std::get_if<NameExpr>(&node->receiver->data);

        // A built-in type name used as a namespace (real.atom, real.unary):
        // type names are reserved, so the receiver cannot be a binding.
        if (receiver_name && !receiver_name->this_qualifier && !receiver_name->self_reference) {
            if (const auto target =
                    standard_type_function_target(receiver_name->name, node->method)) {
                for (auto& type_argument : node->type_arguments) {
                    rename_type(type_argument, ns, local_classes, imports, type_parameters);
                }
                rename_call_args(
                    node->args, ns, local_classes, local_functions, imports, type_parameters);
                CallExpr call;
                call.callee = std::string(*target);
                call.args = std::move(node->args);
                call.type_arguments = std::move(node->type_arguments);
                call.mutation_marker = node->mutation_marker;
                expression.data = std::move(call);
                return;
            }
        }

        if (auto local_namespace = expression_qualified_name(*node->receiver)) {
            const std::string local_callee = *local_namespace + "." + node->method;
            if (local_classes.contains(local_callee)) {
                for (auto& type_argument : node->type_arguments) {
                    rename_type(type_argument, ns, local_classes, imports, type_parameters);
                }
                rename_call_args(
                    node->args, ns, local_classes, local_functions, imports, type_parameters);

                CallExpr call;
                call.callee = qualify(ns, local_callee);
                call.args = std::move(node->args);
                call.type_arguments = std::move(node->type_arguments);
                call.mutation_marker = node->mutation_marker;
                expression.data = std::move(call);
                return;
            }
        }

        std::string module_spelling;
        if (const auto* module = expression_namespace(*node->receiver, imports, module_spelling)) {
            {
                for (auto& type_argument : node->type_arguments) {
                    rename_type(type_argument, ns, local_classes, imports, type_parameters);
                }
                rename_call_args(node->args, ns, local_classes, local_functions, imports, type_parameters);

                std::string callee;
                if (const auto fn = module->functions.find(node->method);
                    fn != module->functions.end()) {
                    callee = fn->second;
                } else if (const auto cls = module->classes.find(node->method);
                           cls != module->classes.end()) {
                    callee = cls->second;
                } else if (module->private_names.contains(node->method)) {
                    private_declaration_error(node->method, module_spelling, expression.span);
                } else {
                    frontend_error("UNKNOWN_MODULE_MEMBER",
                                   "Module '" + module_spelling + "' has no exported callable '" +
                                       node->method + "'.",
                                   expression.span);
                }

                CallExpr call;
                call.callee = std::move(callee);
                call.args = std::move(node->args);
                call.type_arguments = std::move(node->type_arguments);
                call.mutation_marker = node->mutation_marker;
                expression.data = std::move(call);
                return;
            }
        }

        if (receiver_name && !receiver_name->this_qualifier && !receiver_name->self_reference &&
            local_classes.contains(receiver_name->name))
            receiver_name->name = qualify(ns, receiver_name->name);
        rename_expr(*node->receiver, ns, local_classes, local_functions, imports, type_parameters);
        for (auto& type_argument : node->type_arguments) {
            rename_type(type_argument, ns, local_classes, imports, type_parameters);
        }
        rename_call_args(node->args, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
}

void reject_import_alias_shadow_stmt(
    const Stmt& statement,
    const std::unordered_set<std::string>& aliases) {
    if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
        if(node->pattern)visit_pattern(*node->pattern,[&](const Pattern& p){if(p.kind==Pattern::Kind::Declare && aliases.contains(p.name))frontend_error("SHADOWING","Binding shadows an import alias '"+p.name+"'.",p.span);});
        if (!node->pattern && aliases.contains(node->name)) {
            frontend_error("SHADOWING", "Binding shadows an import alias '" + node->name + "'.", statement.span);
        }
        return;
    }
    if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
        for (const auto& child : node->then_body) reject_import_alias_shadow_stmt(*child, aliases);
        for (const auto& child : node->else_body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<ParallelStmt>(&statement.data)) {
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
        if(node->pattern)visit_pattern(*node->pattern,[&](const Pattern& p){if(p.kind==Pattern::Kind::Declare && aliases.contains(p.name))frontend_error("SHADOWING","Iteration binding shadows an import alias '"+p.name+"'.",p.span);});
        if (!node->pattern && aliases.contains(node->name)) {
            frontend_error("SHADOWING", "Iteration binding shadows an import alias '" + node->name + "'.", statement.span);
        }
        for(const auto& binding:node->collections)
            if(aliases.contains(binding.name))
                frontend_error("SHADOWING","Iteration binding shadows an import alias '"+binding.name+"'.",binding.span);
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
        for (const auto& match_case : node->cases) {
            if(match_case.pattern)visit_pattern(*match_case.pattern,[&](const Pattern& p){if(p.kind==Pattern::Kind::Declare && aliases.contains(p.name))frontend_error("SHADOWING","Case binding shadows an import alias '"+p.name+"'.",p.span);});
            if (match_case.binder && aliases.contains(*match_case.binder)) {
                frontend_error("SHADOWING",
                               "Match binding shadows an import alias '" + *match_case.binder + "'.",
                               match_case.span);
            }
            for (const auto& child : match_case.body) reject_import_alias_shadow_stmt(*child, aliases);
        }
    }
}

void reject_import_alias_shadow_function(
    const FunctionDecl& function,
    const std::unordered_set<std::string>& aliases) {
    for (const auto& parameter : function.parameters) {
        if (aliases.contains(parameter.name)) {
            frontend_error("SHADOWING",
                           "Parameter shadows an import alias '" + parameter.name + "'.",
                           parameter.span);
        }
    }
    for (const auto& statement : function.body) {
        reject_import_alias_shadow_stmt(*statement, aliases);
    }
}

void reject_import_alias_shadowing(
    const Program& program,
    const std::unordered_set<std::string>& aliases) {
    for (const auto& class_decl : program.classes) {
        for (const auto& field : class_decl.fields) {
            if (aliases.contains(field.name)) {
                frontend_error("SHADOWING",
                               "Class field shadows an import alias '" + field.name + "'.",
                               field.span);
            }
        }
        for (const auto& method : class_decl.methods) {
            if (aliases.contains(method.name)) {
                frontend_error("SHADOWING",
                               "Method shadows an import alias '" + method.name + "'.",
                               method.span);
            }
            reject_import_alias_shadow_function(method, aliases);
        }
    }
    for (const auto& function : program.functions) {
        reject_import_alias_shadow_function(function, aliases);
    }
    for (const auto& statement : program.statements) {
        reject_import_alias_shadow_stmt(*statement, aliases);
    }
}

void rename_stmt(
    Stmt& statement,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_set<std::string>& local_functions,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& type_parameters) {
    const auto rename_pattern=[&](Pattern& pattern){visit_pattern(pattern,[&](Pattern& p){if(p.type)rename_type(*p.type,ns,local_classes,imports,type_parameters);if(p.place)rename_expr(*p.place,ns,local_classes,local_functions,imports,type_parameters);});};
    if (auto* node = std::get_if<BindingStmt>(&statement.data)) {
        if(node->pattern)rename_pattern(*node->pattern);
        rename_type(node->declared_type, ns, local_classes, imports, type_parameters);
        if (node->value) rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        if (node->literal_constant) {
            if (!node->value) literal_constant_error(statement.span);
            (void)clone_literal_initializer(*node->value, node->value->span);
            if (!literal_constant_scopes.empty())
                literal_constant_scopes.back()[node->name] = node->value.get();
        }
        return;
    }
    if (auto* node = std::get_if<AssignStmt>(&statement.data)) {
        if(node->pattern)rename_pattern(*node->pattern);
        if(node->target)rename_expr(*node->target, ns, local_classes, local_functions, imports, type_parameters);
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<RebindStmt>(&statement.data)) {
        if(node->place)rename_expr(*node->place, ns, local_classes, local_functions, imports, type_parameters);
        if(node->target)rename_expr(*node->target, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<ReturnStmt>(&statement.data)) {
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (std::holds_alternative<LoopControlStmt>(statement.data)) return;
    if (auto* node = std::get_if<ExprStmt>(&statement.data)) {
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<IfStmt>(&statement.data)) {
        rename_expr(*node->condition, ns, local_classes, local_functions, imports, type_parameters);
        {
            LiteralConstantScope scope;
            for (auto& child : node->then_body) {
                rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
            }
        }
        LiteralConstantScope scope;
        for (auto& child : node->else_body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
        LiteralConstantScope scope;
        for (auto& child : node->body)
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<ParallelStmt>(&statement.data)) {
        LiteralConstantScope scope;
        for (auto& child : node->body)
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<WhileStmt>(&statement.data)) {
        rename_expr(*node->condition, ns, local_classes, local_functions, imports, type_parameters);
        LiteralConstantScope scope;
        for (auto& child : node->body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<ForStmt>(&statement.data)) {
        visit_for_header(*node,[&](Expr& operand) {
            rename_expr(operand,ns,local_classes,local_functions,imports,type_parameters);
        });
        if(node->type) rename_type(*node->type,ns,local_classes,imports,type_parameters);
        for(auto& binding:node->collections) if(binding.type) rename_type(*binding.type,ns,local_classes,imports,type_parameters);
        if(node->pattern)visit_pattern(*node->pattern,[&](Pattern& p){if(p.type)rename_type(*p.type,ns,local_classes,imports,type_parameters);if(p.place)rename_expr(*p.place,ns,local_classes,local_functions,imports,type_parameters);});
        LiteralConstantScope scope;
        for (auto& child : node->body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    auto& node = std::get<MatchStmt>(statement.data);
    rename_expr(*node.value, ns, local_classes, local_functions, imports, type_parameters);
    for (auto& match_case : node.cases) {
        if(match_case.pattern)visit_pattern(*match_case.pattern,[&](Pattern& p){if(p.type)rename_type(*p.type,ns,local_classes,imports,type_parameters);});
        else rename_type(match_case.type, ns, local_classes, imports, type_parameters);
        LiteralConstantScope scope;
        for (auto& child : match_case.body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
    }
}

void rename_function(
    FunctionDecl& function,
    const std::string& ns,
    const std::unordered_set<std::string>& local_classes,
    const std::unordered_set<std::string>& local_functions,
    const std::unordered_map<std::string, ImportBinding>& imports,
    const std::unordered_set<std::string>& inherited_type_parameters = {}) {
    std::unordered_set<std::string> type_parameters = inherited_type_parameters;
    type_parameters.insert(function.type_parameters.begin(), function.type_parameters.end());
    // A function sees its own contextual constants only.
    auto outer = std::move(literal_constant_scopes);
    literal_constant_scopes.clear();
    struct RestoreScopes {
        decltype(outer)& saved;
        ~RestoreScopes() { literal_constant_scopes = std::move(saved); }
    } restore{outer};
    LiteralConstantScope scope;

    rename_type(function.return_type, ns, local_classes, imports, type_parameters);
    for (auto& parameter : function.parameters) {
        rename_type(parameter.type, ns, local_classes, imports, type_parameters);
        if (parameter.default_value) {
            rename_expr(*parameter.default_value, ns, local_classes, local_functions, imports, type_parameters);
        }
    }
    for (auto& statement : function.body) {
        rename_stmt(*statement, ns, local_classes, local_functions, imports, type_parameters);
    }
}


std::string canonical_signature_type(const TypeName& type, const FunctionDecl& function);
DeclarationIdentity original_declaration_identity(const FunctionDecl&, std::string package,
    std::string module_path, std::string owner_class, std::string source_name,
    const std::vector<std::string>& class_parameters = {},
    const std::vector<std::string>& class_constraints = {});


bool same_parameter_contract(const Parameter& a, const Parameter& b, std::string_view source) {
    (void)source;
    return a.name == b.name && a.writable == b.writable &&
           a.is_const == b.is_const && a.is_extension == b.is_extension &&
           a.type.name == b.type.name;
}

std::string function_prototype_key(const FunctionDecl& function, std::string_view source) {
    (void)source;
    std::string key = function.name + "<";
    for (std::size_t i = 0; i < function.type_parameters.size(); ++i) {
        if (i) key += ",";
        key += function.type_parameters[i];
        if (i < function.type_constraints.size() &&
            !function.type_constraints[i].empty()) {
            key += ":";
            key += function.type_constraints[i];
        }
    }
    key += ">->";
    key += canonical_signature_type(function.return_type, function);
    key += "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) key += ",";
        const auto& parameter = function.parameters[i];
        if (parameter.is_extension) key += "extend ";
        if (parameter.is_const) key += "const ";
        key += canonical_signature_type(parameter.type, function);
        if (parameter.writable) key += "&";
        key += " ";
        key += parameter.name;
    }
    key += ")";
    return key;
}

// The names a source file declares, as written, checked before anything
// qualifies or instantiates them: `_` is the discard name and names nothing,
// and names that start with "__quidra_" are the compiler's (the instances it
// makes of generic declarations are named so). Bindings inside bodies and
// match binders are the checker's.
void validate_declared_name(const std::string& name, SourceSpan span,
                            bool typed_binding = false) {
    if (is_discard_name(name)) {
        frontend_error("DISCARD",
                       typed_binding ? discard_typed_message : discard_declaration_message,
                       span);
    }
    if (name.starts_with(abi::symbol_namespace::internal_prefix)) {
        frontend_error("SHADOWING", compiler_name_message(name), span);
    }
}

void validate_declared_names(const std::vector<std::string>& type_parameters, SourceSpan span) {
    for (const auto& parameter : type_parameters) validate_declared_name(parameter, span);
}

void validate_declared_names(const FunctionDecl& function) {
    validate_declared_name(function.name, function.span);
    validate_declared_names(function.type_parameters, function.span);
    for (const auto& parameter : function.parameters) {
        validate_declared_name(parameter.name, parameter.span);
    }
}

void validate_declared_names(const Program& program) {
    for (const auto& import_decl : program.imports) {
        validate_declared_name(import_decl.alias, import_decl.span);
    }
    for (const auto& class_decl : program.classes) {
        validate_declared_name(class_decl.name, class_decl.span);
        validate_declared_names(class_decl.type_parameters, class_decl.span);
        for (const auto& field : class_decl.fields) validate_declared_name(field.name, field.span);
        for (const auto& method : class_decl.methods) validate_declared_names(method);
    }
    for (const auto& enum_decl : program.enums) {
        validate_declared_name(enum_decl.name, enum_decl.span);
        for (const auto& variant : enum_decl.variants) {
            validate_declared_name(variant.name, variant.span);
        }
    }
    for (const auto& function : program.functions) validate_declared_names(function);
    for (const auto& statement : program.statements) {
        if (const auto* binding = std::get_if<BindingStmt>(&statement->data)) {
            validate_declared_name(binding->name, statement->span, true);
        }
    }
}

// The targets of `export "C"`, checked on the declarations as written, before
// generic declarations are instantiated (a generic one is never in the
// checked program) and before names are qualified: the C symbol is the name
// as declared. The types, forms and symbols are the checker's.
void validate_foreign_exports(Program& program) {
    for (const auto& class_decl : program.classes) {
        for (const auto& method : class_decl.methods) {
            if (!method.foreign_export) continue;
            frontend_error("FFI_EXPORT",
                           method.is_constructor
                               ? "export \"C\" applies only to top-level free functions; constructors cannot be exported."
                               : "export \"C\" applies only to top-level free functions; methods cannot be exported.",
                           method.foreign_export->span);
        }
    }
    for (auto& function : program.functions) {
        if (!function.foreign_export) continue;
        if (!function.type_parameters.empty()) {
            frontend_error("FFI_EXPORT",
                           "Generic functions cannot be exported with the C ABI; export a concrete wrapper.",
                           function.foreign_export->span);
        }
        if (function.foreign_export->abi != "C") {
            frontend_error("FFI_EXPORT", "Only the \"C\" ABI can be exported.",
                           function.foreign_export->span);
        }
        function.foreign_export->symbol = function.name;
    }
}

// L3: a private class or enum is usable only inside its module, so no
// public declaration may expose it to importers: the parameters or result of
// a public function, a public field of a public class, a payload of a public
// enum, or the type of a public constant (PRIVATE_DECLARATION).
void reject_private_exposure(const Program& program) {
    std::unordered_map<std::string, const char*> private_types;
    for (const auto& declaration : program.classes)
        if (declaration.is_private) private_types.emplace(declaration.name, "class");
    for (const auto& declaration : program.enums)
        if (declaration.is_private) private_types.emplace(declaration.name, "enum");
    if (private_types.empty()) return;
    const std::function<const std::string*(const TypeName&)> exposed =
        [&](const TypeName& type) -> const std::string* {
        if (const auto found = private_types.find(type.name); found != private_types.end())
            return &found->first;
        if (const auto dot = type.name.find('.'); dot != std::string::npos) {
            // An enum variant spelled through its enum.
            if (const auto found = private_types.find(type.name.substr(0, dot));
                found != private_types.end())
                return &found->first;
        }
        for (const auto& argument : type.arguments)
            if (const auto* name = exposed(argument)) return name;
        for (const auto& parameter : type.function_parameters)
            if (const auto* name = exposed(parameter)) return name;
        return nullptr;
    };
    const auto report = [&](const std::string& what, const std::string& type_name,
                            SourceSpan span) {
        frontend_error("PRIVATE_DECLARATION",
                       what + " exposes private " + private_types.at(type_name) + " '" +
                           type_name + "'.",
                       span);
    };
    for (const auto& function : program.functions) {
        if (function.is_private) continue;
        const auto what = std::string("Public function '") + function.name + "'";
        if (const auto* name = exposed(function.return_type))
            report(what, *name, function.return_type.span);
        for (const auto& parameter : function.parameters)
            if (const auto* name = exposed(parameter.type)) report(what, *name, parameter.type.span);
    }
    for (const auto& declaration : program.classes) {
        if (declaration.is_private) continue;
        for (const auto& field : declaration.fields) {
            if (field.is_private) continue;
            if (const auto* name = exposed(field.type))
                report("Public field '" + field.name + "' of class '" + declaration.name + "'",
                       *name, field.type.span);
        }
    }
    for (const auto& declaration : program.enums) {
        if (declaration.is_private) continue;
        for (const auto& variant : declaration.variants) {
            if (!variant.payload) continue;
            if (const auto* name = exposed(*variant.payload))
                report("Public enum '" + declaration.name + "'", *name, variant.payload->span);
        }
    }
    for (const auto& statement : program.statements) {
        const auto* binding = std::get_if<BindingStmt>(&statement->data);
        if (!binding || !binding->is_const || binding->is_private || binding->is_local) continue;
        if (const auto* name = exposed(binding->declared_type))
            report("Public constant '" + binding->name + "'", *name, binding->declared_type.span);
    }
}

void resolve_local_prototypes(Program& program, std::string_view source) {
    std::unordered_map<std::string, std::vector<std::size_t>> functions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        if (!program.functions[i].external_symbol) {
            functions[function_prototype_key(program.functions[i], source)].push_back(i);
        }
    }
    std::vector<bool> remove_functions(program.functions.size());
    for (const auto& [signature, indices] : functions) {
        (void)signature;
        std::vector<std::size_t> prototypes, definitions;
        for (const auto index : indices)
            (program.functions[index].is_prototype ? prototypes : definitions).push_back(index);
        if (prototypes.empty()) continue;
        const auto& name = program.functions[prototypes.front()].name;
        if (prototypes.size() != 1 || definitions.size() != 1) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Function prototype '" + name + "' requires exactly one later definition with the same complete signature in the same source file.",
                           program.functions[prototypes.front()].span);
        }
        auto& prototype = program.functions[prototypes.front()];
        auto& definition = program.functions[definitions.front()];
        if (prototype.span.start.offset >= definition.span.start.offset) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Function prototype '" + name + "' must precede its definition.", prototype.span);
        }
        if (prototype.is_private != definition.is_private) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Function prototype '" + name +
                               "' and its definition must agree on 'private'.",
                           definition.span);
        }
        if (prototype.mutation_marker != definition.mutation_marker) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Function prototype '" + name +
                               "' and its definition must agree on the '.' marker.",
                           definition.span);
        }
        if (prototype.foreign_export.has_value() != definition.foreign_export.has_value()) {
            const auto& marked = prototype.foreign_export ? prototype : definition;
            frontend_error("FFI_EXPORT",
                           "A prototype and its definition must both carry export \"C\".",
                           marked.foreign_export->span);
        }
        if (prototype.type_parameters != definition.type_parameters ||
            prototype.type_constraints != definition.type_constraints ||
            canonical_signature_type(prototype.return_type, prototype) != canonical_signature_type(definition.return_type, definition) ||
            prototype.parameters.size() != definition.parameters.size()) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Function prototype and definition must have the same complete signature.",
                           definition.span);
        }
        for (std::size_t i = 0; i < prototype.parameters.size(); ++i) {
            if (!same_parameter_contract(prototype.parameters[i], definition.parameters[i], source)) {
                frontend_error("PROTOTYPE_MISMATCH",
                               "Function prototype and definition parameter contracts differ.",
                               definition.parameters[i].span);
            }
            if (definition.parameters[i].default_value) {
                frontend_error("PROTOTYPE_MISMATCH",
                               "Defaults belong on the prototype only when a prototype exists.",
                               definition.parameters[i].default_value->span);
            }
            definition.parameters[i].default_value = std::move(prototype.parameters[i].default_value);
        }
        definition.prototype_span = prototype.span;
        remove_functions[prototypes.front()] = true;
    }
    std::vector<FunctionDecl> kept_functions;
    kept_functions.reserve(program.functions.size());
    for (std::size_t i = 0; i < program.functions.size(); ++i)
        if (!remove_functions[i]) kept_functions.push_back(std::move(program.functions[i]));
    program.functions = std::move(kept_functions);

    std::unordered_map<std::string, std::vector<std::size_t>> classes;
    for (std::size_t i = 0; i < program.classes.size(); ++i) classes[program.classes[i].name].push_back(i);
    std::vector<bool> remove_classes(program.classes.size());
    for (const auto& [name, indices] : classes) {
        std::vector<std::size_t> prototypes, definitions;
        for (const auto index : indices)
            (program.classes[index].is_prototype ? prototypes : definitions).push_back(index);
        if (prototypes.empty()) continue;
        if (prototypes.size() != 1 || definitions.size() != 1) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Class prototype '" + name + "' requires exactly one later definition in the same source file.",
                           program.classes[prototypes.front()].span);
        }
        auto& prototype = program.classes[prototypes.front()];
        auto& definition = program.classes[definitions.front()];
        if (prototype.span.start.offset >= definition.span.start.offset ||
            prototype.type_parameters != definition.type_parameters ||
            prototype.type_constraints != definition.type_constraints) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Class prototype and definition must have the same generic contract and appear in that order.",
                           definition.span);
        }
        if (prototype.is_private != definition.is_private) {
            frontend_error("PROTOTYPE_MISMATCH",
                           "Class prototype '" + name + "' and its definition must agree on 'private'.",
                           definition.span);
        }
        const auto& prototype_initializer = prototype.methods.front();
        auto& definition_initializer = definition.methods.front();
        if (prototype.self_name != definition.self_name ||
            canonical_signature_type(prototype_initializer.return_type, prototype_initializer) !=
                canonical_signature_type(definition_initializer.return_type, definition_initializer) ||
            prototype_initializer.parameters.size() != definition_initializer.parameters.size()) {
            frontend_error("PROTOTYPE_MISMATCH", "Class prototype and definition must have the same generation contract.", definition.span);
        }
        for (std::size_t i = 0; i < prototype_initializer.parameters.size(); ++i) {
            auto& parameter = definition_initializer.parameters[i];
            const auto& expected = prototype_initializer.parameters[i];
            if (!same_parameter_contract(expected, parameter, source) || expected.is_local != parameter.is_local ||
                canonical_signature_type(expected.type, prototype_initializer) !=
                    canonical_signature_type(parameter.type, definition_initializer))
                frontend_error("PROTOTYPE_MISMATCH", "Class prototype and definition argument storage contracts differ.", parameter.span);
            if (parameter.default_value)
                frontend_error("PROTOTYPE_MISMATCH", "Defaults belong on the class prototype only when a prototype exists.", parameter.default_value->span);
            parameter.default_value = std::move(prototype.methods.front().parameters[i].default_value);
        }
        definition.prototype_span = prototype.span;
        remove_classes[prototypes.front()] = true;
    }
    std::vector<ClassDecl> kept_classes;
    kept_classes.reserve(program.classes.size());
    for (std::size_t i = 0; i < program.classes.size(); ++i)
        if (!remove_classes[i]) kept_classes.push_back(std::move(program.classes[i]));
    program.classes = std::move(kept_classes);
}


class SourceOrderValidator {
public:
    explicit SourceOrderValidator(const Program& program) : program_(program) {
        for (const auto& function : program_.functions) {
            const auto exposure = function.prototype_span
                ? function.prototype_span->start.offset
                : function.span.start.offset;
            const auto existing = function_exposure_.find(function.name);
            if (existing == function_exposure_.end()) {
                function_exposure_[function.name] = exposure;
            } else {
                existing->second = std::min(existing->second, exposure);
            }
        }
        for (const auto& declaration : program_.classes) {
            if (declaration.name.rfind("$cli.", 0) == 0) {
                class_exposure_[declaration.name] = 0;
                class_completion_[declaration.name] = 0;
                continue;
            }
            const auto exposure = declaration.prototype_span
                ? declaration.prototype_span->start.offset
                : declaration.span.start.offset;
            const auto [exposure_entry, exposure_inserted] =
                class_exposure_.try_emplace(declaration.name, exposure);
            if (!exposure_inserted) {
                exposure_entry->second = std::min(exposure_entry->second, exposure);
            }
            const auto completion = declaration.span.start.offset;
            const auto [completion_entry, completion_inserted] =
                class_completion_.try_emplace(declaration.name, completion);
            if (!completion_inserted) {
                completion_entry->second = std::min(completion_entry->second, completion);
            }
        }
    }

    void run() const {
        for (const auto& declaration : program_.enums) {
            for (const auto& variant : declaration.variants) {
                if (variant.payload)
                    check_type(*variant.payload, variant.span.start.offset, true);
            }
        }
        for (const auto& declaration : program_.classes) {
            for (const auto& field : declaration.fields) {
                check_type(field.type, field.span.start.offset, true);
                if (field.default_value) check_expr(*field.default_value, {});
            }
            for (const auto& method : declaration.methods) {
                for (const auto& parameter : method.parameters)
                    check_type(parameter.type, method.span.start.offset, false);
                check_type(method.return_type, method.span.start.offset, false);
                for (const auto& parameter : method.parameters)
                    if (parameter.default_value) check_expr(*parameter.default_value, {});
                for (const auto& statement : method.body)
                    check_stmt(*statement, {});
            }
        }
        for (const auto& function : program_.functions) {
            const auto interface_offset = function.prototype_span
                ? function.prototype_span->start.offset
                : function.span.start.offset;
            for (const auto& parameter : function.parameters)
                check_type(parameter.type, interface_offset, false);
            check_type(function.return_type, interface_offset, false);
            for (const auto& parameter : function.parameters)
                if (parameter.default_value) check_expr(*parameter.default_value, function.name);
            for (const auto& statement : function.body)
                check_stmt(*statement, function.name);
        }
        for (const auto& statement : program_.statements) check_stmt(*statement, {});
    }

private:
    const Program& program_;
    std::unordered_map<std::string, std::size_t> function_exposure_;
    std::unordered_map<std::string, std::size_t> class_exposure_;
    std::unordered_map<std::string, std::size_t> class_completion_;

    void check_type(const TypeName& type, std::size_t offset, bool complete) const {
        if (const auto found = class_exposure_.find(type.name); found != class_exposure_.end()) {
            const auto required = complete ? class_completion_.at(type.name) : found->second;
            if (offset < required) {
                frontend_error(
                    "SOURCE_ORDER",
                    complete
                        ? "Class '" + type.name + "' is not complete at this source position."
                        : "Class '" + type.name + "' is not visible yet; add a class prototype above this use.",
                    type.span);
            }
        }
        for (const auto& argument : type.arguments) check_type(argument, offset, complete);
        for (const auto& parameter : type.function_parameters) check_type(parameter, offset, complete);
    }

    void check_call_name(std::string_view name, SourceSpan span,
                         std::string_view current_function) const {
        if (const auto found = function_exposure_.find(std::string(name));
            found != function_exposure_.end() && name != current_function &&
            span.start.offset < found->second) {
            frontend_error(
                "SOURCE_ORDER",
                "Function '" + std::string(name) +
                    "' is not visible yet; add its prototype above this use.",
                span);
        }
        if (const auto found = class_completion_.find(std::string(name));
            found != class_completion_.end() && span.start.offset < found->second) {
            frontend_error(
                "SOURCE_ORDER",
                "Class '" + std::string(name) +
                    "' is incomplete here; construction requires its full definition.",
                span);
        }
    }

    void check_expr(const Expr& expression, std::string_view current_function) const {
        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (name->this_qualifier || name->self_reference) return;
            if (const auto found = function_exposure_.find(name->name);
                found != function_exposure_.end() && name->name != current_function &&
                expression.span.start.offset < found->second) {
                frontend_error(
                    "SOURCE_ORDER",
                    "Function '" + name->name +
                        "' is not visible yet; add its prototype above this use.",
                    expression.span);
            }
            return;
        }
        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            check_call_name(call->callee, expression.span, current_function);
            for (const auto& type : call->type_arguments)
                check_type(type, expression.span.start.offset, false);
            for (const auto& argument : call->args)
                if (argument.value) check_expr(*argument.value, current_function);
            return;
        }
        if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
            check_expr(*call->receiver, current_function);
            for (const auto& type : call->type_arguments)
                check_type(type, expression.span.start.offset, false);
            for (const auto& argument : call->args)
                if (argument.value) check_expr(*argument.value, current_function);
            return;
        }
        if (const auto* node = std::get_if<TupleExpr>(&expression.data)) {
            for (const auto& item : node->elements) check_expr(*item, current_function);
            return;
        }
        if (const auto* node = std::get_if<ArrayComprehensionExpr>(&expression.data)) {
            if (node->type) check_type(*node->type, node->binding_span.start.offset, false);
            visit_array_comprehension(*node, [&](const Expr& child) { check_expr(child, current_function); });
            return;
        }
        if (const auto* node = std::get_if<ArrayExpr>(&expression.data)) {
            for (const auto& item : node->elements) check_expr(*item, current_function);
            return;
        }
        if (const auto* node = std::get_if<IndexExpr>(&expression.data)) {
            check_expr(*node->base, current_function);
            for (const auto& item : node->items) {
                if (item.index) check_expr(*item.index, current_function);
                if (item.start) check_expr(*item.start, current_function);
                if (item.stop) check_expr(*item.stop, current_function);
                if (item.step) check_expr(*item.step, current_function);
            }
            return;
        }
        if (const auto* node = std::get_if<MemberExpr>(&expression.data)) {
            check_expr(*node->base, current_function);
            return;
        }
        if (const auto* node = std::get_if<UnaryExpr>(&expression.data)) {
            check_expr(*node->operand, current_function);
            return;
        }
        if (const auto* node = std::get_if<BinaryExpr>(&expression.data)) {
            check_expr(*node->left, current_function);
            check_expr(*node->right, current_function);
            return;
        }
        if (const auto* node = std::get_if<ComparisonChainExpr>(&expression.data)) {
            for (const auto& operand : node->operands) check_expr(*operand, current_function);
            return;
        }
        if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            for (const auto& condition : node->conditions) check_expr(*condition, current_function);
            for (const auto& value : node->values) check_expr(*value, current_function);
            check_expr(*node->otherwise, current_function);
            return;
        }
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) check_expr(*item, current_function);
        }
    }

    void check_stmt(const Stmt& statement, std::string_view current_function) const {
        std::visit([&](const auto& node){if constexpr(requires {node.pattern;})if(node.pattern)visit_pattern(*node.pattern,[&](const Pattern& p){if(p.type && p.type->name!="auto")check_type(*p.type,p.span.start.offset,true);if(p.place)check_expr(*p.place,current_function);});},statement.data);
        if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
            if (node->declared_type.name != "auto")
                check_type(node->declared_type, statement.span.start.offset, true);
            if (node->value) check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            if(node->target)check_expr(*node->target, current_function);
            check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            if(node->place)check_expr(*node->place, current_function);
            if(node->target)check_expr(*node->target, current_function);
            return;
        }
        if (const auto* node = std::get_if<ReturnStmt>(&statement.data)) {
            if (node->value) check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<ExprStmt>(&statement.data)) {
            if (node->value) check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<IfStmt>(&statement.data)) {
            check_expr(*node->condition, current_function);
            for (const auto& child : node->then_body) check_stmt(*child, current_function);
            for (const auto& child : node->else_body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<ParallelStmt>(&statement.data)) {
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            check_expr(*node->condition, current_function);
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            visit_for_header(*node,[&](const Expr& operand) { check_expr(operand,current_function); });
            if(node->type) check_type(*node->type,node->type->span.start.offset,false);
            for(const auto& binding:node->collections) if(binding.type) check_type(*binding.type,binding.type->span.start.offset,false);
            if(node->pattern)visit_pattern(*node->pattern,[&](const Pattern& p){if(p.type)check_type(*p.type,p.span.start.offset,false);if(p.place)check_expr(*p.place,current_function);});
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            check_expr(*node->value, current_function);
            for (const auto& match_case : node->cases) {
                if(match_case.pattern)visit_pattern(*match_case.pattern,[&](const Pattern& p){if(p.type)check_type(*p.type,p.span.start.offset,false);});
                else check_type(match_case.type, match_case.span.start.offset, false);
                for (const auto& child : match_case.body) check_stmt(*child, current_function);
            }
        }
    }
};

class ModuleLoader {
public:
    ModuleLoader(
        fs::path cwd, std::size_t max_errors,
        std::optional<std::string> root_source = std::nullopt,
        std::map<std::string, fs::path>* resolved_packages = nullptr,
        bool enforce_package_lock = true,
        std::vector<CompilerExtensionRegistration>* compiler_extensions = nullptr,
        CompileInputs* inputs = nullptr)
        : cwd_(fs::absolute(std::move(cwd)).lexically_normal()),
          max_errors_(max_errors ? max_errors : 1),
          root_source_(std::move(root_source)),
          resolved_packages_(resolved_packages),
          enforce_package_lock_(enforce_package_lock),
          compiler_extensions_(compiler_extensions),
          inputs_(inputs) {}

    Program load(const fs::path& root) {
        Program merged;
        const auto absolute_root = root.is_absolute()
            ? root.lexically_normal()
            : (cwd_ / root).lexically_normal();
        merged.root_source_file = absolute_root.string();
        if (enforce_package_lock_) {
            try {
                package_lock_ = read_package_lock(cwd_, inputs_);
            } catch (const std::exception& error) {
                frontend_error("PACKAGE_LOCK", error.what());
            }
            package_lock_loaded_ = true;
        }
        load_file(root, "", true, true, merged);
        if (package_lock_) {
            for (const auto& [name, _] : *package_lock_) {
                if (!loaded_packages_.contains(name)) {
                    frontend_error(
                        "PACKAGE_LOCK_UNUSED",
                        "Package '" + name +
                            "' is recorded in " +
                            std::string(package_lock_filename) +
                            " but is not reached by the current import graph. "
                            "Regenerate it with 'quidra lock FILE" +
                            std::string(source_extension) + "'.");
                }
            }
        }
        semantics::finalize_original_interfaces(*interface_sources_);
        merged.interface_sources = interface_sources_;
        return merged;
    }

private:
    fs::path cwd_;
    std::size_t max_errors_{20};
    std::optional<std::string> root_source_;
    std::map<std::string, fs::path>* resolved_packages_{};
    bool enforce_package_lock_{true};
    std::vector<CompilerExtensionRegistration>* compiler_extensions_{};
    CompileInputs* inputs_{};
    bool package_lock_loaded_{};
    std::optional<PackageLockEntries> package_lock_;
    std::unordered_map<std::string, std::string> package_hash_cache_;
    std::map<std::string, fs::path> loaded_packages_;
    std::vector<fs::path> stack_;
    std::unordered_set<std::string> loaded_standard_declarations_;
    std::shared_ptr<semantics::InterfaceSources> interface_sources_{std::make_shared<semantics::InterfaceSources>()};
    std::map<std::string,std::size_t> interface_namespaces_;

    void record_package(
        const std::string& name, const fs::path& package_main, SourceSpan span) {
        const auto normalized = fs::absolute(package_main).lexically_normal();
        const auto [loaded, inserted] = loaded_packages_.emplace(name, normalized);
        if (!inserted && loaded->second != normalized) {
            frontend_error(
                "PACKAGE_RESOLUTION_CONFLICT",
                "Package '" + name + "' resolved to more than one installed location.",
                span);
        }
        if (resolved_packages_) {
            resolved_packages_->emplace(name, normalized);
        }

        std::optional<PackageManifest> manifest;
        try {
            manifest = try_read_package_manifest(normalized.parent_path(), inputs_);
            if (manifest) {
                if (manifest->name != name) {
                    frontend_error(
                        "PACKAGE_MANIFEST",
                        "Package '" + name + "' has manifest name '" +
                            manifest->name + "'.",
                        span);
                }

                if (const auto requirement =
                        manifest->requirements.find("quidra");
                    requirement != manifest->requirements.end() &&
                    !requirement->second.matches(
                        parse_semantic_version(version))) {
                    frontend_error(
                        "PACKAGE_COMPATIBILITY",
                        "Package '" + name + "' " + manifest->version.str() +
                            " requires Quidra " + requirement->second.text +
                            "; current Quidra is " +
                            std::string(version) + ".",
                        span);
                }

                if (manifest->project &&
                    manifest->project->abi_requirement &&
                    *manifest->project->abi_requirement !=
                        static_cast<unsigned long long>(abi_version)) {
                    frontend_error(
                        "PACKAGE_COMPATIBILITY",
                        "Package '" + name + "' requires Quidra ABI " +
                            std::to_string(*manifest->project->abi_requirement) +
                            "; current compiler provides ABI " +
                            std::to_string(abi_version) + ".",
                        span);
                }

                for (const auto& [dependency_name, requirement] :
                     manifest->requirements) {
                    if (dependency_name == "quidra") continue;

                    std::optional<fs::path> dependency_main;
                    try {
                        dependency_main =
                            resolve_installed_package_path(dependency_name, inputs_);
                    } catch (const std::exception& error) {
                        frontend_error("PACKAGE_DEPENDENCY", error.what(), span);
                    }
                    if (!dependency_main) {
                        frontend_error(
                            "PACKAGE_DEPENDENCY",
                            "Package '" + name + "' " + manifest->version.str() +
                                " requires package '" + dependency_name + "' " +
                                requirement.text + ", but it is not installed.",
                            span);
                    }

                    try {
                        const auto dependency_manifest =
                            try_read_package_manifest(
                                dependency_main->parent_path(), inputs_);
                        if (!dependency_manifest) {
                            frontend_error(
                                "PACKAGE_DEPENDENCY",
                                "Package '" + name + "' " +
                                    manifest->version.str() +
                                    " requires versioned package '" +
                                    dependency_name + "' " + requirement.text +
                                    ", but the installed dependency has no "
                                    "quidra.package manifest.",
                                span);
                        }
                        if (!requirement.matches(
                                dependency_manifest->version)) {
                            frontend_error(
                                "PACKAGE_DEPENDENCY",
                                "Package '" + name + "' " +
                                    manifest->version.str() +
                                    " requires package '" + dependency_name +
                                    "' " + requirement.text +
                                    "; installed version is " +
                                    dependency_manifest->version.str() + ".",
                                span);
                        }
                    } catch (const CompileError&) {
                        throw;
                    } catch (const std::exception& error) {
                        frontend_error("PACKAGE_DEPENDENCY", error.what(), span);
                    }
                }
            }
        } catch (const CompileError&) {
            throw;
        } catch (const std::exception& error) {
            frontend_error("PACKAGE_MANIFEST", error.what(), span);
        }

        if (inserted && manifest && compiler_extensions_) {
            try {
                for (const auto& [extension_name, descriptor_path] :
                     package_compiler_extension_paths(
                         normalized.parent_path(), *manifest)) {
                    const auto descriptor = read_text(
                        descriptor_path, InputFileKind::descriptor, inputs_);
                    compiler_extensions_->push_back(parse_compiler_extension_descriptor(
                        name, extension_name, normalized.parent_path(), descriptor_path,
                        descriptor, inputs_));
                }
            } catch (const std::exception& error) {
                frontend_error(
                    "PACKAGE_COMPILER_EXTENSION", error.what(), span);
            }
        }

        if (!enforce_package_lock_) return;

        if (!package_lock_loaded_) {
            try {
                package_lock_ = read_package_lock(cwd_, inputs_);
            } catch (const std::exception& error) {
                frontend_error("PACKAGE_LOCK", error.what(), span);
            }
            package_lock_loaded_ = true;
        }
        if (!package_lock_) return;

        const auto expected = package_lock_->find(name);
        if (expected == package_lock_->end()) {
            frontend_error(
                "PACKAGE_LOCK_MISSING",
                "Package '" + name +
                    "' is imported but is not recorded in " +
                    std::string(package_lock_filename) +
                    ". Regenerate it with 'quidra lock FILE" +
                    std::string(source_extension) + "'.",
                span);
        }

        if (expected->second.distribution_name_explicit) {
            const std::string actual_distribution_name = manifest
                ? std::string(package_distribution_name(*manifest))
                : name;
            if (actual_distribution_name !=
                expected->second.distribution_name) {
                frontend_error(
                    "PACKAGE_LOCK_IDENTITY",
                    "Installed package '" + name +
                        "' has distribution identity '" +
                        actual_distribution_name +
                        "', but quidra.lock records '" +
                        expected->second.distribution_name + "'.",
                    span);
            }
        }

        if (expected->second.version != "-") {
            if (!manifest ||
                manifest->version.str() != expected->second.version) {
                frontend_error(
                    "PACKAGE_LOCK_VERSION",
                    "Installed package '" + name +
                        "' does not match version " +
                        expected->second.version +
                        " recorded in quidra.lock.",
                    span);
            }
        }

        const auto cache_key = normalized.string();
        auto actual = package_hash_cache_.find(cache_key);
        if (actual == package_hash_cache_.end()) {
            try {
                actual = package_hash_cache_
                    .emplace(cache_key, package_tree_sha256(normalized))
                    .first;
            } catch (const std::exception& error) {
                frontend_error("PACKAGE_LOCK", error.what(), span);
            }
            if (inputs_) inputs_->record(PackageTreeInput{normalized, actual->second});
        }

        if (actual->second != expected->second.sha256) {
            frontend_error(
                "PACKAGE_LOCK_MISMATCH",
                "Installed package '" + name +
                    "' does not match the SHA-256 recorded in quidra.lock. "
                    "Reinstall the locked package or intentionally regenerate the lockfile.",
                span);
        }
    }

    fs::path logical_path(const std::string& target) const {
        fs::path relative = target;
        if (relative.extension().empty()) {
            relative += std::string(source_extension);
        }
        if (relative.is_absolute()) {
            frontend_error("IMPORT_PATH", "Logical module names resolve inside the command working directory.");
        }
        const auto normalized = relative.lexically_normal();
        if (!normalized.empty() && *normalized.begin() == "..") {
            frontend_error("IMPORT_PATH", "Logical module name cannot escape the command working directory.");
        }
        return (cwd_ / normalized).lexically_normal();
    }

    Exports load_file(const fs::path& source_path, const std::string& ns, bool root, bool user,
                      Program& merged) {
        const auto absolute = source_path.is_absolute()
            ? source_path.lexically_normal()
            : (cwd_ / source_path).lexically_normal();

        if (std::find(stack_.begin(), stack_.end(), absolute) != stack_.end()) {
            frontend_error("IMPORT_CYCLE", "Module import cycle includes '" + absolute.string() + "'.");
        }

        std::string source;
        if (root && root_source_) {
            source = *root_source_;
        } else {
            try {
                source = read_text(absolute, InputFileKind::source, inputs_);
            } catch (const std::exception&) {
                frontend_error("IMPORT_IO", "Cannot read imported module '" + absolute.string() + "'.");
            }
        }

        const auto source_file = absolute.string();
        merged.source_texts[source_file] = source;
        if (user && std::find(merged.user_sources.begin(), merged.user_sources.end(),
                              source_file) == merged.user_sources.end()) {
            merged.user_sources.push_back(source_file);
        }

        auto tokens = Lexer(source).scan();
        std::unordered_set<std::string> referenced_standard_modules;
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind == TokenKind::Identifier &&
                is_standard_module(tokens[i].text) &&
                tokens[i + 1].kind == TokenKind::Dot) {
                referenced_standard_modules.insert(tokens[i].text);
            }
        }
        Parser parser(std::move(tokens), max_errors_);
        Program program = parser.parse();
        validate_declared_names(program);
        validate_foreign_exports(program);
        resolve_local_prototypes(program, source);
        SourceOrderValidator(program).run();
        for (auto& function : program.functions) function.source_file = source_file;
        std::string identity_package, identity_module;
        std::size_t identity_root_size = 0;
        for (const auto& [package, main] : loaded_packages_) {
            const auto relative = absolute.lexically_relative(main.parent_path());
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") continue;
            const auto root_size = main.parent_path().native().size();
            if (root_size < identity_root_size) continue;
            identity_root_size = root_size; identity_package = package;
            if (absolute == main) identity_module.clear();
            else {
                auto module = relative; module.replace_extension();
                identity_module = module.generic_string();
                std::replace(identity_module.begin(), identity_module.end(), '/', '.');
            }
        }
        if (identity_package.empty()) identity_module = ns;
        const auto record_identity = [&](FunctionDecl& function, const std::string& owner,
            const std::string& source_name, const auto& class_parameters, const auto& class_constraints) {
            function.declaration = original_declaration_identity(function, identity_package,
                identity_module, owner, source_name, class_parameters, class_constraints);
            if (std::none_of(merged.declaration_catalog.begin(), merged.declaration_catalog.end(),
                            [&](const auto& existing) { return same_declaration(existing, *function.declaration); }))
                merged.declaration_catalog.push_back(*function.declaration);
        };
        for (auto& function : program.functions)
            record_identity(function, {}, function.name, std::vector<std::string>{}, std::vector<std::string>{});
        for (auto& declaration : program.enums) declaration.source_file = source_file;
        for (auto& declaration : program.classes) {
            declaration.source_file = source_file;
            std::size_t constructor = 0;
            for (auto& method : declaration.methods) {
                method.source_file = source_file;
                record_identity(method, declaration.name,
                    method.is_constructor ? std::to_string(constructor++) : method.name,
                    declaration.type_parameters, declaration.type_constraints);
            }
        }

        const auto interface_index = interface_sources_->modules.size();
        interface_sources_->modules.push_back(semantics::capture_original_interfaces(
            program, {identity_package,identity_module}, source,
            [](const TypeName& type) {
                FunctionDecl signature;
                signature.return_type = type;
                return canonical_signature_type(type,signature);
            }));
        interface_namespaces_[ns] = interface_index;
        for (const auto& binding : interface_sources_->modules[interface_index].bindings)
            semantics::bind_interface_name(interface_sources_->modules[interface_index],binding.kind,
                binding.original_name,qualify(ns,binding.original_name));

        for (const auto& enum_decl : program.enums) {
            if ((root && is_reserved_value_name(enum_decl.name)) || enum_decl.name == "main")
                frontend_error("DUPLICATE_NAME", "Enum name '" + enum_decl.name + "' is reserved.", enum_decl.span);
        }
        for (const auto& class_decl : program.classes) {
            if (class_decl.name.rfind("$cli.", 0) == 0) continue;
            if ((root && is_reserved_value_name(class_decl.name)) || class_decl.name == "main") {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Class name '" + class_decl.name + "' is reserved.",
                    class_decl.span);
            }
        }
        for (const auto& function : program.functions) {
            if ((root && is_reserved_value_name(function.name)) || function.name == "main") {
                frontend_error(
                    function.name == "main" ? "RESERVED_MAIN" : "DUPLICATE_NAME",
                    "Function name '" + function.name + "' is reserved.",
                    function.span);
            }
        }

        if (!root) {
            for (const auto& statement : program.statements) {
                if (std::holds_alternative<MainGuardStmt>(statement->data)) continue;
                if (const auto* binding = std::get_if<BindingStmt>(&statement->data);
                    binding && binding->is_const && !binding->reference && binding->value) {
                    if (binding->literal_constant)
                        (void)clone_literal_initializer(*binding->value, binding->value->span);
                    else
                        (void)clone_package_constant_expression(
                            *binding->value, binding->value->span);
                    continue;
                }
                frontend_error(
                    "IMPORT_TOP_LEVEL",
                    "Imported modules may contain declarations, immutable compile-time const bindings, and 'if main' guards only; other top-level executable statements belong in the root file.",
                    statement->span);
            }
        }
        for (auto& statement : program.statements) {
            if (auto* guard = std::get_if<MainGuardStmt>(&statement->data)) {
                guard->active = root;
                guard->source_file = source_file;
                guard->module_namespace = ns;
            }
        }

        std::unordered_set<std::string> local_classes;
        std::unordered_set<std::string> local_functions;
        std::unordered_set<std::string> local_constants;
        for (const auto& class_decl : program.classes) local_classes.insert(class_decl.name);
        for (const auto& enum_decl : program.enums) local_classes.insert(enum_decl.name);
        for (const auto& function : program.functions) local_functions.insert(function.name);
        for (const auto& statement : program.statements) {
            if (const auto* binding = std::get_if<BindingStmt>(&statement->data);
                binding && binding->is_const) {
                local_constants.insert(binding->name);
            }
        }

        reject_private_exposure(program);
        Exports exports;
        for (const auto& class_decl : program.classes) {
            if (class_decl.is_private) {
                exports.private_names.insert(class_decl.name);
                continue;
            }
            insert_class_export(
                exports, class_decl.name, qualify(ns, class_decl.name), class_decl.span);
        }
        for (const auto& enum_decl : program.enums) {
            if (enum_decl.is_private) {
                exports.private_names.insert(enum_decl.name);
                continue;
            }
            exports.enums[enum_decl.name] = qualify(ns, enum_decl.name);
        }
        for (const auto& function : program.functions) {
            if (function.is_private) exports.private_names.insert(function.name);
            else exports.functions[function.name] = qualify(ns, function.name);
        }
        if (!root) {
            for (const auto& statement : program.statements) {
                const auto* binding = std::get_if<BindingStmt>(&statement->data);
                if (!binding || !binding->is_const || !binding->value || binding->is_local) continue;
                if (binding->is_private) {
                    exports.private_names.insert(binding->name);
                    continue;
                }
                if (exports.classes.contains(binding->name) ||
                    exports.enums.contains(binding->name) ||
                    exports.functions.contains(binding->name) ||
                    exports.values.contains(binding->name) ||
                    exports.namespaces.contains(binding->name) ||
                    exports.constants.contains(binding->name)) {
                    frontend_error(
                        "DUPLICATE_NAME",
                        "Const export '" + binding->name +
                            "' conflicts with another exported declaration.",
                        statement->span);
                }
                auto value = binding->literal_constant
                    ? literal_constant_use(binding->name, *binding->value, binding->value->span)
                    : clone_package_constant_expression(*binding->value, binding->value->span);
                if (!binding->literal_constant && binding->declared_type.name != "auto") {
                    value->contextual_default_type = binding->declared_type;
                }
                exports.constants.emplace(
                    binding->name, std::shared_ptr<Expr>(value.release()));
            }
        }

        std::unordered_map<std::string, ImportBinding> imports;
        std::unordered_set<std::string> aliases;

        for (const auto module_name : standard_modules) {
            const std::string module(module_name);
            auto module_exports = standard_exports(module, standard_span());
            if (referenced_standard_modules.contains(module) &&
                loaded_standard_declarations_.insert(module).second) {
                auto declarations = standard_declarations(module);
                for (auto& declaration : declarations) {
                    // Language-owned declarations live in their standard
                    // namespace, never in the user program's scope.
                    declaration.module_namespace = "$std." + module;
                    for (auto& method : declaration.methods)
                        method.module_namespace = declaration.module_namespace;
                    merged.classes.push_back(std::move(declaration));
                }
            }
            imports.emplace(module, ImportBinding{"$std." + module, std::move(module_exports)});
            aliases.insert(module);
        }

        stack_.push_back(absolute);
        for (const auto& import_decl : program.imports) {
            if (!import_decl.local_path && is_standard_module(import_decl.target)) {
                stack_.pop_back();
                frontend_error(
                    "STANDARD_NAMESPACE_IMPORT",
                    "Standard namespace '" + import_decl.target +
                        "' is always available and cannot be imported.",
                    import_decl.span);
            }
            if (is_reserved_value_name(import_decl.alias) ||
                import_decl.alias == "main") {
                stack_.pop_back();
                frontend_error(
                    "SHADOWING",
                    "Import alias '" + import_decl.alias + "' shadows a reserved visible name.",
                    import_decl.span);
            }
            if (!aliases.insert(import_decl.alias).second ||
                local_classes.contains(import_decl.alias) ||
                local_functions.contains(import_decl.alias) ||
                local_constants.contains(import_decl.alias)) {
                stack_.pop_back();
                frontend_error("DUPLICATE_IMPORT_ALIAS",
                               "Import alias '" + import_decl.alias + "' conflicts with another visible declaration.",
                               import_decl.span);
            }

            if (!import_decl.local_path) {
                std::optional<fs::path> package_path;
                try {
                    package_path =
                        resolve_installed_package_path(import_decl.target, inputs_);
                } catch (const std::invalid_argument& error) {
                    stack_.pop_back();
                    frontend_error("PACKAGE_IMPORT", error.what(), import_decl.span);
                }
                if (!package_path) {
                    stack_.pop_back();
                    frontend_error(
                        "PACKAGE_NOT_INSTALLED",
                        "Package '" + import_decl.target +
                            "' is not installed. Package imports never fall back to the current directory.",
                        import_decl.span);
                }
                record_package(import_decl.target, *package_path, import_decl.span);
                const auto child_ns = qualify(ns, import_decl.alias);
                auto child_exports = load_file(*package_path, child_ns, false, false, merged);
                semantics::bind_interface_import(interface_sources_->modules[interface_index],
                    import_decl.alias,interface_sources_->modules.at(interface_namespaces_.at(child_ns)).module);
                if (import_decl.is_public) {
                    exports.namespaces[import_decl.alias] = std::make_shared<Exports>(child_exports);
                }
                imports.emplace(
                    import_decl.alias, ImportBinding{child_ns, std::move(child_exports)});
                continue;
            }

            fs::path target;
            try {
                target = resolve_local_import_path(
                    import_decl.target, absolute, cwd_).path;
            } catch (const std::invalid_argument& error) {
                stack_.pop_back();
                frontend_error("IMPORT_PATH", error.what(), import_decl.span);
            }
            if (inputs_) inputs_->record(LocalImportInput{absolute, import_decl.target, target});

            const auto child_ns = qualify(ns, import_decl.alias);
            auto child_exports = load_file(target, child_ns, false, user, merged);
            semantics::bind_interface_import(interface_sources_->modules[interface_index],
                import_decl.alias,interface_sources_->modules.at(interface_namespaces_.at(child_ns)).module);
            if (import_decl.is_public) {
                exports.namespaces[import_decl.alias] = std::make_shared<Exports>(child_exports);
            }
            imports.emplace(import_decl.alias, ImportBinding{child_ns, std::move(child_exports)});
        }
        stack_.pop_back();

        // Keep the resolved lexical import graph until generic expansion has
        // resolved extension calls. Qualifying each alias records its loaded
        // namespace and its parent scope without changing import visibility.
        for (const auto& import_decl : program.imports) {
            auto resolved_import = import_decl;
            resolved_import.alias = qualify(ns, import_decl.alias);
            merged.imports.push_back(std::move(resolved_import));
        }

        reject_import_alias_shadowing(program, aliases);

        for (auto& enum_decl : program.enums) {
            for (auto& variant : enum_decl.variants)
                if (variant.payload) rename_type(*variant.payload, ns, local_classes, imports, {});
            enum_decl.name = qualify(ns, enum_decl.name);
        }

        for (auto& class_decl : program.classes) {
            std::unordered_set<std::string> class_parameters(
                class_decl.type_parameters.begin(), class_decl.type_parameters.end());

            for (auto& field : class_decl.fields) {
                rename_type(field.type, ns, local_classes, imports, class_parameters);
                if (field.default_value) {
                    rename_expr(*field.default_value, ns, local_classes, local_functions, imports, class_parameters);
                }
            }
            for (auto& method : class_decl.methods) {
                rename_function(
                    method, ns, local_classes, local_functions, imports, class_parameters);
                method.module_namespace = ns;
            }
            class_decl.name = qualify(ns, class_decl.name);
            class_decl.module_namespace = ns;
        }

        for (auto& function : program.functions) {
            rename_function(function, ns, local_classes, local_functions, imports);
            function.name = qualify(ns, function.name);
            function.module_namespace = ns;
        }

        {
            LiteralConstantScope scope;
            for (auto& statement : program.statements) {
                if (root || std::holds_alternative<MainGuardStmt>(statement->data))
                    rename_stmt(*statement, ns, local_classes, local_functions, imports, {});
            }
        }
        if (!root) {
            // Keep imported declarations in the semantic program while marking their top-level
            // spans as non-root. Source inspection/patching can then remain root-file scoped.
            const auto imported_offset = std::numeric_limits<std::size_t>::max();
            for (auto& enum_decl : program.enums) {
                enum_decl.span.start.offset = imported_offset;
                enum_decl.span.end.offset = imported_offset;
            }
            for (auto& class_decl : program.classes) {
                class_decl.span.start.offset = imported_offset;
                class_decl.span.end.offset = imported_offset;
            }
            for (auto& function : program.functions) {
                function.span.start.offset = imported_offset;
                function.span.end.offset = imported_offset;
            }
        }

        for (auto& enum_decl : program.enums) merged.enums.push_back(std::move(enum_decl));
        for (auto& class_decl : program.classes) merged.classes.push_back(std::move(class_decl));
        for (auto& function : program.functions) merged.functions.push_back(std::move(function));
        for (auto& statement : program.statements) {
            if (root || std::holds_alternative<MainGuardStmt>(statement->data))
                merged.statements.push_back(std::move(statement));
        }

        return exports;
    }
};

TypeName clone_type(const TypeName& source) {
    TypeName out;
    out.name = source.name;
    out.array_depth = source.array_depth;
    out.dimensions = source.dimensions;
    out.dimension_expressions = source.dimension_expressions;
    out.span = source.span;
    out.tensor_shape_prefix = source.tensor_shape_prefix;
    out.tensor_shape_ellipsis = source.tensor_shape_ellipsis;
    out.tensor_shape_expressions = source.tensor_shape_expressions;
    out.tensor_rank = source.tensor_rank;
    out.tensor_known_shape_prefix = source.tensor_known_shape_prefix;
    out.brace_shape = source.brace_shape;
    out.shape_axes = source.shape_axes;
    for (const auto& argument : source.arguments) out.arguments.push_back(clone_type(argument));
    for (const auto& parameter : source.function_parameters)
        out.function_parameters.push_back(clone_type(parameter));
    return out;
}

// The spelling of an extent expression in an identity key. A shape variable
// of the type being keyed prints as `rename` maps it (its number or `_`); a
// conversion nat(e) prints e, which is the value it captures.
std::string canonical_extent_expression(
    const Expr& expression,
    const std::unordered_map<std::string, std::string>* rename = nullptr) {
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        return std::to_string(literal->value);
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (!name->this_qualifier && !name->self_reference && rename) {
            if (const auto found = rename->find(name->name); found != rename->end())
                return found->second;
        }
        return name->this_qualifier ? "this." + name->name : name->name;
    }
    if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        if (call->callee == "nat" && call->type_arguments.empty() && call->args.size() == 1 &&
            !call->args[0].name && !call->args[0].writable) {
            return canonical_extent_expression(*call->args[0].value, rename);
        }
        std::string out = call->callee + "(";
        for (std::size_t i = 0; i < call->args.size(); ++i) {
            if (i) out += ",";
            out += canonical_extent_expression(*call->args[i].value, rename);
        }
        return out + ")";
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        return unary->op + "(" + canonical_extent_expression(*unary->operand, rename) + ")";
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
        return "(" + canonical_extent_expression(*binary->left, rename) + binary->op +
               canonical_extent_expression(*binary->right, rename) + ")";
    }
    return "<extent>";
}

void count_extent_names(const Expr& expression, std::unordered_map<std::string, int>& counts) {
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        if (!name->this_qualifier && !name->self_reference) {
            if (auto found = counts.find(name->name); found != counts.end()) ++found->second;
        }
    } else if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
        for (const auto& argument : call->args) count_extent_names(*argument.value, counts);
    } else if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        count_extent_names(*unary->operand, counts);
    } else if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
        count_extent_names(*binary->left, counts);
        count_extent_names(*binary->right, counts);
    }
}

// The identity key of a brace shape (L17): constraints, never names. A
// shape variable that occurs once constrains nothing and prints `_`, as an
// unconstrained axis does; one that occurs more than once prints `$k` at
// every occurrence, numbered by first occurrence; `nat N = EXPR` and an
// unnamed expression print their expression; a bare existing value prints
// its name. So {nat K, K} is <$0,$0>, {nat A, nat B} and {_, _} are <_,_>,
// and a migrated {nat(n), 2} keeps the key <n,2> of today's <n, 2>.
std::string brace_shape_key(const std::vector<ShapeAxis>& axes,
                            const std::unordered_map<std::string, std::string>* shared = nullptr,
                            std::optional<std::size_t> ellipsis = {}) {
    std::unordered_map<std::string, int> counts;
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual)
            counts.emplace(axis.name, 0);
    }
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual) {
            if (auto found = counts.find(axis.name); found != counts.end()) ++found->second;
        }
        if (axis.value) count_extent_names(*axis.value, counts);
    }
    std::unordered_map<std::string, std::string> rename;
    int next = 0;
    // Numbered by first occurrence: a declaration or a bare reference.
    const auto number = [&](const std::string& name) {
        if (rename.contains(name)) return;
        const auto found = counts.find(name);
        if (found == counts.end()) return;
        rename[name] = found->second > 1 ? "$" + std::to_string(next++) : "_";
    };
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual ||
            axis.kind == ShapeAxisKind::Bare) {
            number(axis.name);
        }
    }
    // A `nat N = EXPR` variable stands for its expression.
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::DeclareEqual && axis.value)
            rename[axis.name] = canonical_extent_expression(*axis.value, &rename);
    }
    if (shared) rename = *shared;
    std::string out = "<";
    bool first_axis = true;
    for (std::size_t i = 0; i <= axes.size(); ++i) {
        if (ellipsis == i) {
            if (!first_axis) out += ",";
            out += "...";
            first_axis = false;
        }
        if (i == axes.size()) break;
        if (!first_axis) out += ",";
        const auto& axis = axes[i];
        switch (axis.kind) {
            case ShapeAxisKind::Discard: out += "_"; break;
            case ShapeAxisKind::Declare:
            case ShapeAxisKind::DeclareEqual:
            case ShapeAxisKind::Bare: {
                const auto found = rename.find(axis.name);
                out += found != rename.end() ? found->second : axis.name;
                break;
            }
            case ShapeAxisKind::Unnamed:
                out += axis.value ? canonical_extent_expression(*axis.value, &rename) : "_";
                break;
        }
        first_axis = false;
    }
    return out + ">";
}

std::unordered_map<std::string, std::string> signature_shape_rename(const FunctionDecl& function) {
    std::vector<ShapeAxis> axes;
    const auto append = [&](const TypeName& type) {
        if (type.brace_shape) axes.insert(axes.end(), type.shape_axes.begin(), type.shape_axes.end());
    };
    append(function.return_type);
    for (const auto& parameter : function.parameters) append(parameter.type);
    std::unordered_map<std::string, int> counts;
    std::unordered_map<std::string, const Expr*> derived;
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual) {
            ++counts[axis.name];
            if (axis.kind == ShapeAxisKind::DeclareEqual) derived[axis.name] = axis.value.get();
        }
    }
    for (const auto& axis : axes) if (axis.value) count_extent_names(*axis.value, counts);
    std::unordered_map<std::string, std::string> rename;
    int next = 0;
    const auto number = [&](const std::string& name) {
        if (!rename.contains(name) && counts.contains(name))
            rename[name] = counts.at(name) > 1 ? "$" + std::to_string(next++) : "_";
    };
    for (const auto& axis : axes) {
        if (axis.kind == ShapeAxisKind::Declare || axis.kind == ShapeAxisKind::DeclareEqual ||
            axis.kind == ShapeAxisKind::Bare) number(axis.name);
    }
    // Derived axes may reference declarations later in the signature.
    std::unordered_map<std::string, int> state;
    std::function<void(const std::string&)> derive = [&](const std::string& name) {
        if (!derived.contains(name) || state[name] == 2) return;
        if (state[name] == 1) return; // The checker reports the cycle.
        state[name] = 1;
        std::unordered_map<std::string, int> references;
        for (const auto& [variable, unused] : derived) {
            (void)unused; references.emplace(variable, 0);
        }
        count_extent_names(*derived.at(name), references);
        for (const auto& [variable, count] : references) if (count) derive(variable);
        rename[name] = canonical_extent_expression(*derived.at(name), &rename);
        state[name] = 2;
    };
    for (const auto& axis : axes) if (derived.contains(axis.name)) derive(axis.name);
    for (std::size_t p = 0; p < function.parameters.size(); ++p)
        if (!rename.contains(function.parameters[p].name))
            rename[function.parameters[p].name] = "#parameter" + std::to_string(p);
    return rename;
}

std::string canonical_type(const TypeName& type,
                           const std::unordered_map<std::string, std::string>* shapes = nullptr) {
    // A built-in type is named by its canonical spelling, whichever alias
    // the source used (int and int64 are one type).
    std::string out(canonical_type_name(type.name));
    if (!type.arguments.empty()) {
        out += "<";
        for (std::size_t i = 0; i < type.arguments.size(); ++i) {
            if (i) out += ",";
            out += canonical_type(type.arguments[i], shapes);
        }
        out += ">";
    }
    if (type.name == "func") {
        out += "(";
        for (std::size_t i = 0; i < type.function_parameters.size(); ++i) {
            if (i) out += ",";
            out += canonical_type(type.function_parameters[i], shapes);
        }
        out += ")";
    }
    if (type.brace_shape && !(type.tensor_shape_ellipsis && type.shape_axes.empty())) {
        out += brace_shape_key(type.shape_axes, shapes, type.tensor_shape_ellipsis);
    } else if (!type.tensor_shape_prefix.empty()) {
        out += "<";
        bool first_axis = true;
        for (std::size_t i = 0; i <= type.tensor_shape_prefix.size(); ++i) {
            if (type.tensor_shape_ellipsis == i) {
                if (!first_axis) out += ",";
                out += "...";
                first_axis = false;
            }
            if (i == type.tensor_shape_prefix.size()) break;
            if (!first_axis) out += ",";
            if (i < type.tensor_shape_expressions.size() &&
                type.tensor_shape_expressions[i]) {
                out += canonical_extent_expression(*type.tensor_shape_expressions[i], shapes);
            } else {
                const auto extent = type.tensor_shape_prefix[i];
                out += extent < 0 ? "_" : std::to_string(extent);
            }
            first_axis = false;
        }
        out += ">";
    }
    for (std::size_t i = 0; i < type.dimensions.size(); ++i) {
        out += "[";
        if (i < type.dimension_expressions.size() && type.dimension_expressions[i]) {
            out += canonical_extent_expression(*type.dimension_expressions[i]);
        } else if (type.dimensions[i] >= 0) {
            out += std::to_string(type.dimensions[i]);
        }
        out += "]";
    }
    return out;
}

std::string canonical_signature_type(const TypeName& type, const FunctionDecl& function) {
    const auto shapes = signature_shape_rename(function);
    return canonical_type(type, &shapes);
}

DeclarationIdentity original_declaration_identity(const FunctionDecl& function, std::string package,
    std::string module_path, std::string owner_class, std::string source_name,
    const std::vector<std::string>& class_parameters, const std::vector<std::string>& class_constraints) {
    DeclarationIdentity result;
    result.kind = function.is_constructor ? DeclarationIdentity::Kind::Constructor :
        owner_class.empty() ? DeclarationIdentity::Kind::Function : DeclarationIdentity::Kind::Method;
    result.package = std::move(package); result.module_path = std::move(module_path);
    result.owner_class = std::move(owner_class); result.source_name = std::move(source_name);
    result.source_file = function.source_file;
    result.generic_parameters = class_parameters;
    result.generic_constraints = class_constraints;
    result.generic_constraints.resize(result.generic_parameters.size());
    result.generic_parameters.insert(result.generic_parameters.end(), function.type_parameters.begin(), function.type_parameters.end());
    for (std::size_t i = 0; i < function.type_parameters.size(); ++i)
        result.generic_constraints.push_back(i < function.type_constraints.size() ? function.type_constraints[i] : "");
    result.signature = "(";
    for (const auto& parameter : function.parameters) {
        if (!result.parameters.empty()) result.signature += ",";
        const auto type = canonical_signature_type(parameter.type, function);
        result.parameters.push_back({parameter.name, type, parameter.writable, parameter.is_const});
        if (parameter.is_const) result.signature += "const ";
        result.signature += type;
        if (parameter.writable) result.signature += "&";
    }
    result.signature += ")";
    result.result_type = canonical_signature_type(function.return_type, function);
    result.mutation_marker = function.mutation_marker; result.external = function.external_symbol.has_value();
    return result;
}



std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t value = 1469598103934665603ULL;
    for (unsigned char c : text) {
        value ^= c;
        value *= 1099511628211ULL;
    }
    return value;
}

std::string short_hash(std::string_view text) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << fnv1a(text);
    return out.str();
}

bool contains_parameter(const TypeName& type, const std::unordered_set<std::string>& parameters) {
    if (parameters.contains(type.name)) return true;
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       [&](const auto& argument) { return contains_parameter(argument, parameters); }) ||
           std::any_of(type.function_parameters.begin(), type.function_parameters.end(),
                       [&](const auto& parameter) { return contains_parameter(parameter, parameters); });
}

bool standard_collection_key_type(const TypeName& type) {
    if(type.name=="$tuple" && type.dimensions.empty())return std::all_of(type.arguments.begin(),type.arguments.end(),standard_collection_key_type);
    if (!type.arguments.empty() || !type.dimensions.empty()) return false;
    if (const auto* numeric = numeric_type_name(type.name)) {
        return numeric->family == NumericFamily::Integer ||
               numeric->family == NumericFamily::Natural;
    }
    return type.name == "bool" || type.name == "string";
}

std::optional<std::vector<StmtPtr>> standard_collection_hash_body(
    const TypeName& type,
    const std::string& parameter) {
    if(type.name=="$tuple" && type.dimensions.empty()) {
        std::ostringstream source;
        const auto spelling=[&](const auto& self,const TypeName& t)->std::string {if(t.name!="$tuple")return t.name;std::string name="(";for(std::size_t i=0;i<t.arguments.size();++i){if(i)name+=", ";name+=self(self,t.arguments[i]);}return name+")";};
        source<<"int64 __hash("<<spelling(spelling,type)<<" "<<parameter<<")\n";
        std::size_t serial=0;
        const auto hash=[&](const auto& self,const TypeName& t,const std::string& value)->std::string {
            const auto id=std::to_string(serial++);const auto result="__tuple_hash_"+id;
            if(t.name=="$tuple") {
                std::vector<std::string> names;source<<"    (";for(std::size_t i=0;i<t.arguments.size();++i){if(i)source<<", ";names.push_back("__tuple_key_"+id+"_"+std::to_string(i));source<<spelling(spelling,t.arguments[i])<<" "<<names.back();}source<<") = "<<value<<"\n";
                source<<"    int64 "<<result<<" = 0\n";
                for(std::size_t i=0;i<t.arguments.size();++i){const auto child=self(self,t.arguments[i],names[i]);source<<"    "<<result<<" = ("<<result<<" * 1000003 + ("<<child<<" % 2147483647)) % 2147483647\n";}
            } else if(t.name=="string") {
                source<<"    int[] __tuple_units_"<<id<<" = "<<value<<".codepoints()\n    int64 "<<result<<" = 0\n    for auto __tuple_unit_"<<id<<" = __tuple_units_"<<id<<"[]\n        int64 __tuple_unit64_"<<id<<" = int64(__tuple_unit_"<<id<<")\n        "<<result<<" = ("<<result<<" * 131 + __tuple_unit64_"<<id<<") % 2147483647\n";
            } else if(t.name=="bool") {
                source<<"    int64 "<<result<<" = 0\n    if "<<value<<"\n        "<<result<<" = 1\n";
            } else {
                const auto* numeric=numeric_type_name(t.name);
                if(numeric->kind==TypeKind::Int || numeric->kind==TypeKind::Nat){source<<"    int __tuple_mod_"<<id<<" = int("<<value<<") % 2147483647\n    if __tuple_mod_"<<id<<" < 0\n        __tuple_mod_"<<id<<" += 2147483647\n    int64 "<<result<<" = int64(__tuple_mod_"<<id<<")\n";}
                else if(numeric->kind==TypeKind::Nat64)source<<"    int64 "<<result<<" = int64("<<value<<" AND nat64(9223372036854775807))\n";
                else source<<"    int64 "<<result<<" = int64("<<value<<") AND 9223372036854775807\n";
            }
            return result;
        };
        const auto result=hash(hash,type,parameter);source<<"    return "<<result<<"\n";
        Parser parser(Lexer(source.str()).scan(),20);auto program=parser.parse();if(program.functions.size()!=1)throw std::logic_error("invalid tuple hash specialization");return std::move(program.functions.front().body);
    }
    if (!type.arguments.empty() || !type.dimensions.empty()) return std::nullopt;

    std::ostringstream source;
    const auto* numeric = numeric_type_name(type.name);
    const auto kind = numeric ? numeric->kind : TypeKind::Invalid;
    // The hash of a key is an int64, as the synthesized collections keep
    // their slots and hashes.
    if (type.name == "bool") {
        source << "int64 __hash(bool " << parameter << ")\n"
               << "    if " << parameter << "\n"
               << "        return 1\n"
               << "    return 0\n";
    } else if (
        kind == TypeKind::Int64 || kind == TypeKind::Int8 ||
        kind == TypeKind::Int16 || kind == TypeKind::Int32) {
        source << "int64 __hash(" << numeric->spelling << " " << parameter << ")\n"
               << "    int64 __value = int64(" << parameter << ")\n"
               << "    return __value AND 9223372036854775807\n";
    } else if (
        kind == TypeKind::Nat8 || kind == TypeKind::Nat16 ||
        kind == TypeKind::Nat32) {
        source << "int64 __hash(" << numeric->spelling << " " << parameter << ")\n"
               << "    return int64(" << parameter << ")\n";
    } else if (kind == TypeKind::Nat64) {
        source << "int64 __hash(nat64 " << parameter << ")\n"
               << "    nat64 __result = " << parameter
               << " AND nat64(9223372036854775807)\n"
               << "    int64 __code = int64(__result)\n"
               << "    return __code\n";
    } else if (kind == TypeKind::Int || kind == TypeKind::Nat) {
        source << "int64 __hash(" << numeric->spelling << " " << parameter << ")\n"
               << "    int __result = int(" << parameter << ") % 2147483647\n"
               << "    if __result < 0\n"
               << "        __result += 2147483647\n"
               << "    int64 __code = int64(__result)\n"
               << "    return __code\n";
    } else {
        return std::nullopt;
    }

    Parser parser(Lexer(source.str()).scan(), 20);
    auto program = parser.parse();
    if (program.functions.size() != 1 || !program.classes.empty() ||
        !program.statements.empty()) {
        throw std::logic_error("invalid built-in collection hash specialization");
    }
    return std::move(program.functions.front().body);
}

using Substitution = std::unordered_map<std::string, TypeName>;

TypeName substitute_raw(
    const TypeName& source,
    const Substitution& substitution) {
    if (source.arguments.empty()) {
        if (const auto it = substitution.find(source.name); it != substitution.end()) {
            auto result = clone_type(it->second);
            std::vector<long long> dimensions = source.dimensions;
            dimensions.insert(dimensions.end(), result.dimensions.begin(), result.dimensions.end());
            result.dimensions = std::move(dimensions);
            auto dimension_expressions = source.dimension_expressions;
            dimension_expressions.insert(
                dimension_expressions.end(),
                result.dimension_expressions.begin(),
                result.dimension_expressions.end());
            result.dimension_expressions = std::move(dimension_expressions);
            result.array_depth = result.dimensions.size();
            result.span = source.span;
            return result;
        }
    }

    auto result = clone_type(source);
    result.arguments.clear();
    for (const auto& argument : source.arguments) {
        result.arguments.push_back(substitute_raw(argument, substitution));
    }
    result.function_parameters.clear();
    for (const auto& parameter : source.function_parameters) {
        result.function_parameters.push_back(substitute_raw(parameter, substitution));
    }
    return result;
}

class GenericExpander {
public:
    explicit GenericExpander(Program source) : source_(std::move(source)) {
        output_.root_source_file = source_.root_source_file;
        output_.source_display_path = source_.source_display_path;
        output_.source_texts = source_.source_texts;
        output_.user_sources = source_.user_sources;
        output_.package_directories = source_.package_directories;
        output_.declaration_catalog = source_.declaration_catalog;
        output_.interface_sources = source_.interface_sources;
        build_extension_scopes();
        validate_declarations();

        for (auto& class_decl : source_.classes) {
            if (class_decl.type_parameters.empty()) {
                concrete_classes_[class_decl.name] = &class_decl;
            } else {
                class_templates_[class_decl.name] = &class_decl;
            }
        }
        for (auto& function : source_.functions) {
            function_families_[function.name].push_back(&function);
        }
        for (auto& [name, family] : function_families_) {
            if (family.size() == 1) {
                if (family.front()->type_parameters.empty()) {
                    concrete_functions_[name] = family.front();
                } else {
                    function_templates_[name] = family.front();
                }
            } else {
                specialization_families_[name] = family;
            }
        }
    }

    Program run() {
        for (const auto& declaration : source_.enums) {
            EnumDecl copy;
            copy.name = declaration.name;
            copy.source_file = declaration.source_file;
            copy.span = declaration.span;
            for (const auto& variant : declaration.variants) {
                EnumVariantDecl item;
                item.name = variant.name;
                item.span = variant.span;
                if (variant.payload) item.payload = materialize_type(*variant.payload, {}, {});
                copy.variants.push_back(std::move(item));
            }
            output_.enums.push_back(std::move(copy));
        }
        for (const auto& [name, declaration] : concrete_classes_) {
            materialize_concrete_class(*declaration);
        }

        for (const auto& [name, declaration] : concrete_functions_) {
            function_return_types_[name] = materialize_type(declaration->return_type, {}, {}, true);
        }
        for (const auto& [name, declaration] : concrete_functions_) {
            output_.functions.push_back(clone_function(*declaration, {}, {}, ""));
        }

        type_environment_.clear();
        for (const auto& statement : source_.statements) {
            output_.statements.push_back(clone_stmt(*statement, {}, {}, ""));
        }

        output_.imports.clear();
        return std::move(output_);
    }

private:
    struct MethodSpecializationMember {
        FunctionDecl signature;
        const FunctionDecl* source{};
        Substitution class_substitution;

        MethodSpecializationMember() = default;
        MethodSpecializationMember(const MethodSpecializationMember&) = delete;
        MethodSpecializationMember& operator=(const MethodSpecializationMember&) = delete;
        MethodSpecializationMember(MethodSpecializationMember&& other) noexcept
            : signature(std::move(other.signature)),
              source(other.source),
              class_substitution(std::move(other.class_substitution)) {}
        MethodSpecializationMember& operator=(MethodSpecializationMember&& other) noexcept {
            if (this != &other) {
                signature = std::move(other.signature);
                source = other.source;
                class_substitution = std::move(other.class_substitution);
            }
            return *this;
        }
    };

    // Shared by clone_expr and clone_stmt: they interleave over one AST, and
    // this walk runs before the checker, so no checker budget can protect it.
    std::size_t clone_depth_{};

    static bool known_generic_constraint(std::string_view constraint) {
        return constraint.empty() || constraint == "numeric" || constraint == "integer" ||
               constraint == "floating" || constraint == "ordered" ||
               constraint == "equatable";
    }

    static bool scalar_integer_constraint_type(std::string_view name) {
        const auto* numeric = numeric_type_name(name);
        return numeric && (numeric->family == NumericFamily::Integer ||
                           numeric->family == NumericFamily::Natural);
    }

    static bool scalar_floating_constraint_type(std::string_view name) {
        const auto* numeric = numeric_type_name(name);
        return numeric && numeric->family == NumericFamily::Real && numeric->width != 0;
    }

    // Constraint membership follows the numeric-kind table. Complex
    // values are numeric and equatable, while ordered excludes them.
    static bool scalar_numeric_constraint_type(std::string_view name) {
        const auto* numeric = numeric_type_name(name);
        return numeric != nullptr;
    }

    static bool scalar_ordered_constraint_type(std::string_view name) {
        const auto* numeric=numeric_type_name(name);
        return numeric&&numeric->family!=NumericFamily::Complex;
    }

    static bool scalar_complex_type(std::string_view name) {
        const auto* numeric = numeric_type_name(name);
        return numeric && numeric->family == NumericFamily::Complex;
    }

    const ClassDecl* constraint_class(std::string_view name) const {
        for (const auto& declaration : output_.classes)
            if (declaration.name == name) return &declaration;
        for (const auto& declaration : source_.classes)
            if (declaration.name == name) return &declaration;
        return nullptr;
    }

    bool equatable_constraint_type(
        TypeName type, bool inside_array,
        std::unordered_set<std::string>& visiting) const {
        if (!type.dimensions.empty()) {
            type.dimensions.clear();
            type.dimension_expressions.clear();
            type.array_depth = 0;
            return equatable_constraint_type(std::move(type), true, visiting);
        }
        if(type.name=="$tuple" || type.name=="$union")return std::all_of(type.arguments.begin(),type.arguments.end(),[&](const TypeName& child){return child.name=="none" || equatable_constraint_type(clone_type(child),false,visiting);});
        if (!type.arguments.empty() || !type.function_parameters.empty() ||
            type.name == "tensor" || type.name == "func" ||
            type.name == "union") {
            return false;
        }
        if (scalar_numeric_constraint_type(type.name) || scalar_complex_type(type.name) ||
            type.name == "bool" || type.name == "string" || type.name == "bin" ||
            type.name == "error") {
            return true;
        }
        if (is_unequatable_handle_without_autograd_target(type.name)) {
            return false;
        }
        const auto* declaration = constraint_class(type.name);
        if (!declaration || inside_array) return false;
        if (!visiting.insert(type.name).second) return true;
        for (const auto& field : declaration->fields) {
            if (!equatable_constraint_type(clone_type(field.type), false, visiting)) {
                visiting.erase(type.name);
                return false;
            }
        }
        visiting.erase(type.name);
        return true;
    }

    bool satisfies_generic_constraint(
        TypeName type, std::string_view constraint) const {
        if (constraint.empty()) return true;
        if (constraint == "integer") return type.dimensions.empty() &&
            type.arguments.empty() && type.function_parameters.empty() &&
            scalar_integer_constraint_type(type.name);
        if (constraint == "floating") return type.dimensions.empty() &&
            type.arguments.empty() && type.function_parameters.empty() &&
            scalar_floating_constraint_type(type.name);
        if (constraint == "numeric" || constraint == "ordered")
            return type.dimensions.empty() && type.arguments.empty() &&
                type.function_parameters.empty() &&
                (constraint=="numeric"?scalar_numeric_constraint_type(type.name):
                                       scalar_ordered_constraint_type(type.name));
        if (constraint == "equatable") {
            std::unordered_set<std::string> visiting;
            return equatable_constraint_type(std::move(type), false, visiting);
        }
        return false;
    }

    // `additionally_reserved` holds names that a type parameter may not reuse.
    // They are looked up qualified with `reserved_namespace`. For a top-level
    // generic that is the declaration's own module namespace ("" for the root
    // file), so a package generic is checked against its own module's
    // functions, classes, and enums and never against the importing program.
    void validate_type_parameters(
        const std::vector<std::string>& parameters,
        const std::vector<std::string>& constraints,
        const std::unordered_set<std::string>& additionally_reserved,
        const std::string& reserved_namespace,
        SourceSpan span) const {
        if (!constraints.empty() && constraints.size() != parameters.size()) {
            throw std::logic_error("Generic constraint metadata is misaligned.");
        }
        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            const auto& parameter = parameters[i];
            if (!seen.insert(parameter).second) {
                frontend_error("DUPLICATE_NAME",
                               "Duplicate generic type parameter '" + parameter + "'.",
                               span);
            }
            if (is_language_type_name(parameter) || is_reserved_value_name(parameter) ||
                additionally_reserved.contains(qualify(reserved_namespace, parameter)) ||
                parameter == "main") {
                frontend_error("SHADOWING",
                               "Generic type parameter name '" + parameter + "' is reserved or already visible.",
                               span);
            }
            const auto constraint =
                constraints.empty() ? std::string_view{} : std::string_view(constraints[i]);
            if (!known_generic_constraint(constraint)) {
                frontend_error(
                    "GENERIC_CONSTRAINT",
                    "Unknown generic constraint '" + std::string(constraint) +
                        "' on type parameter '" + parameter + "'.",
                    span);
            }
        }
    }

    void validate_generic_arguments(
        const std::vector<std::string>& parameters,
        const std::vector<std::string>& constraints,
        const std::vector<TypeName>& arguments,
        std::string_view target_kind,
        std::string_view target_name,
        SourceSpan span) const {
        if (constraints.empty()) return;
        for (std::size_t i = 0; i < arguments.size() && i < parameters.size(); ++i) {
            if (constraints[i].empty()) continue;
            if (satisfies_generic_constraint(arguments[i], constraints[i])) continue;
            frontend_error(
                "GENERIC_CONSTRAINT",
                "Type '" + canonical_type(arguments[i]) +
                    "' does not satisfy generic constraint '" + constraints[i] +
                    "' for '" + parameters[i] + "' in " +
                    std::string(target_kind) + " '" + std::string(target_name) + "'.",
                arguments[i].span.start.offset ? arguments[i].span : span);
        }
    }

    // The class header supplies the generation signature. The synthetic
    // initializer returns the class, optionally combined with error.
    void validate_constructor(
        const ClassDecl& class_decl, const FunctionDecl& method) const {
        TypeName self;
        self.name = class_decl.name;
        for (const auto& parameter : class_decl.type_parameters) {
            TypeName argument;
            argument.name = parameter;
            self.arguments.push_back(std::move(argument));
        }
        const auto self_spelling = canonical_type(self);
        TypeName fallible;
        fallible.name = "union";
        fallible.arguments.push_back(clone_type(self));
        TypeName error_type;
        error_type.name = "error";
        fallible.arguments.push_back(std::move(error_type));
        const auto declared = canonical_type(method.return_type);
        if (declared != self_spelling && declared != canonical_type(fallible)) {
            frontend_error("CONSTRUCTOR_SIGNATURE",
                           "Generation of '" + class_decl.name + "' must return '" +
                               self_spelling + "' or '" + self_spelling + " | error'.",
                           method.span);
        }
        if (!method.type_parameters.empty()) {
            frontend_error("CONSTRUCTOR_SIGNATURE",
                           "Class generation uses the type parameters of its class header.",
                           method.span);
        }
    }

    std::unordered_map<std::string, std::string> signature_shape_names(const FunctionDecl& function) const {
        return signature_shape_rename(function);
    }

    std::string generic_parameter_shape(
        const TypeName& source,
        const std::vector<std::string>& parameters,
        const std::unordered_map<std::string, std::string>* shapes = nullptr) const {
        auto type = clone_type(source);
        const auto rewrite = [&](auto&& self, TypeName& current) -> void {
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                if (current.name == parameters[i] && current.arguments.empty()) {
                    current.name = "$" + std::to_string(i);
                    break;
                }
            }
            for (auto& argument : current.arguments) self(self, argument);
            for (auto& parameter : current.function_parameters) self(self, parameter);
        };
        rewrite(rewrite, type);
        return canonical_type(type, shapes);
    }

    std::string generic_call_shape(const FunctionDecl& function) const {
        const auto shapes = signature_shape_names(function);
        std::string key;
        for (const auto& parameter : function.parameters) {
            if (!key.empty()) key += ";";
            if (parameter.is_const) key += "const ";
            key += generic_parameter_shape(parameter.type, function.type_parameters, &shapes);
            if (parameter.writable) key += "&";
            key += " " + parameter.name;
        }
        return key;
    }

    bool concrete_matches_generic_pattern(
        const FunctionDecl& generic,
        const FunctionDecl& concrete) const {
        if (generic.parameters.size() != concrete.parameters.size()) return false;
        const std::unordered_set<std::string> parameters{
            generic.type_parameters.begin(), generic.type_parameters.end()};
        Substitution inferred;
        const auto generic_shapes = signature_shape_names(generic);
        const auto concrete_shapes = signature_shape_names(concrete);
        for (std::size_t i = 0; i < generic.parameters.size(); ++i) {
            const auto& pattern = generic.parameters[i].type;
            const auto& actual = concrete.parameters[i].type;
            if (!infer_generic_pattern(pattern, actual, parameters, inferred)) {
                return false;
            }
            if (canonical_type(substitute_raw(pattern, inferred), &generic_shapes) !=
                canonical_type(actual, &concrete_shapes)) {
                return false;
            }
        }
        return true;
    }

    bool concrete_outside_generic_primitive_domain(
        const FunctionDecl& generic, const FunctionDecl& concrete) const {
        if (generic.parameters.size() != concrete.parameters.size()) return false;
        // This exception needs a closed primitive domain; nominal types,
        // callbacks and other unproved containers retain exact-pattern rules.
        const auto primitive_type = [&](auto&& self, const TypeName& type) -> bool {
            if (!type.function_parameters.empty()) return false;
            if (type.name == "tensor") {
                return type.arguments.size() == 1 &&
                    self(self, type.arguments.front());
            }
            return type.arguments.empty() &&
                (numeric_type_name(type.name) || type.name == "bool" ||
                 type.name == "string" || type.name == "bin" ||
                 type.name == "error" || type.name == "none");
        };
        const std::unordered_set<std::string> parameters{
            generic.type_parameters.begin(), generic.type_parameters.end()};
        const auto generic_shapes = signature_shape_names(generic);
        const auto concrete_shapes = signature_shape_names(concrete);
        Substitution result_arguments;
        std::unordered_set<std::string> conflicting_arguments;
        bool outside = false;
        for (std::size_t i = 0; i < generic.parameters.size(); ++i) {
            const auto& pattern = generic.parameters[i].type;
            const auto& actual = concrete.parameters[i].type;
            if (!primitive_type(primitive_type, actual)) return false;
            // Separate argument occurrences only for the disjointness proof.
            // Keep each argument's wrapper, shape and internal relationship.
            Substitution local;
            if (!infer_generic_pattern(pattern, actual, parameters, local) ||
                canonical_type(substitute_raw(pattern, local), &generic_shapes) !=
                    canonical_type(actual, &concrete_shapes)) return false;
            for (const auto& [name, type] : local) {
                if (!primitive_type(primitive_type, type) ||
                    !type.dimensions.empty() || !type.arguments.empty()) return false;
                const auto found = std::find(generic.type_parameters.begin(),
                    generic.type_parameters.end(), name);
                if (found == generic.type_parameters.end()) return false;
                const auto index = static_cast<std::size_t>(
                    found - generic.type_parameters.begin());
                const auto constraint = function_constraint(generic, index);
                if (!constraint.empty() && known_generic_constraint(constraint) &&
                    !satisfies_generic_constraint(clone_type(type), constraint)) outside = true;
                if (!conflicting_arguments.contains(name)) {
                    const auto previous = result_arguments.find(name);
                    if (previous == result_arguments.end()) {
                        result_arguments.emplace(name, clone_type(type));
                    } else if (canonical_type(previous->second) != canonical_type(type)) {
                        result_arguments.erase(previous);
                        conflicting_arguments.insert(name);
                    }
                }
            }
        }
        // A changed relationship cannot determine a result through an
        // inconsistent variable. Preserve the result pattern as well.
        return outside &&
            canonical_type(substitute_raw(generic.return_type, result_arguments),
                           &generic_shapes) ==
                canonical_type(concrete.return_type, &concrete_shapes);
    }

    static std::string_view function_constraint(
        const FunctionDecl& function, std::size_t index) {
        return function.type_constraints.empty()
            ? std::string_view{}
            : std::string_view(function.type_constraints[index]);
    }

    static bool constraint_strictly_narrower(
        std::string_view narrow, std::string_view broad) {
        if (narrow == broad) return false;
        if (broad.empty()) return !narrow.empty();
        if(narrow=="ordered"&&broad=="numeric")return true;
        if ((narrow == "floating" || narrow == "integer") &&
            (broad == "numeric" || broad == "ordered")) {
            return true;
        }
        return false;
    }

    static bool constraints_disjoint(
        std::string_view left, std::string_view right) {
        return (left == "floating" && right == "integer") ||
               (left == "integer" && right == "floating");
    }

    bool generic_more_specific(
        const FunctionDecl& left, const FunctionDecl& right) const {
        if (left.type_parameters.size() != right.type_parameters.size() ||
            generic_call_shape(left) != generic_call_shape(right)) return false;
        bool strict = false;
        for (std::size_t i = 0; i < left.type_parameters.size(); ++i) {
            const auto a = function_constraint(left, i);
            const auto b = function_constraint(right, i);
            if (a == b) continue;
            if (!constraint_strictly_narrower(a, b)) return false;
            strict = true;
        }
        return strict;
    }

    bool generic_domains_disjoint(
        const FunctionDecl& left, const FunctionDecl& right) const {
        if (left.type_parameters.size() != right.type_parameters.size()) return false;
        if (left.parameters.size() != right.parameters.size()) return false;
        // A tensor and an array are distinct outer value domains regardless of
        // their element types. This admits container implementations in one
        // specialization family without introducing ordinary overloads.
        const auto scalar_parameter = [&](const FunctionDecl& function, const TypeName& type) {
            if (!type.dimensions.empty() || !type.arguments.empty() ||
                !type.function_parameters.empty()) return false;
            const auto parameter = std::find(function.type_parameters.begin(),
                                             function.type_parameters.end(), type.name);
            if (parameter == function.type_parameters.end()) return false;
            const auto constraint = function_constraint(function,
                static_cast<std::size_t>(parameter - function.type_parameters.begin()));
            return constraint == "numeric" || constraint == "ordered" ||
                   constraint == "integer" || constraint == "floating";
        };
        for (std::size_t i = 0; i < left.parameters.size(); ++i) {
            const auto& a = left.parameters[i].type;
            const auto& b = right.parameters[i].type;
            const bool a_tensor = a.name == "tensor" && a.dimensions.empty();
            const bool b_tensor = b.name == "tensor" && b.dimensions.empty();
            if ((a_tensor && !b.dimensions.empty()) ||
                (b_tensor && !a.dimensions.empty())) return true;
            // Numeric constraints describe scalar values, so a scalar member
            // and an array/tensor member cannot match the same call. The array
            // element may itself be an array for recursive package operations.
            if ((scalar_parameter(left, a) && (b_tensor || !b.dimensions.empty())) ||
                (scalar_parameter(right, b) && (a_tensor || !a.dimensions.empty()))) return true;
        }
        if (generic_call_shape(left) != generic_call_shape(right)) return false;
        for (std::size_t i = 0; i < left.type_parameters.size(); ++i) {
            if (constraints_disjoint(
                    function_constraint(left, i), function_constraint(right, i))) {
                return true;
            }
        }
        return false;
    }

    std::string concrete_signature_key(const FunctionDecl& function) const {
        const auto shapes = signature_shape_names(function);
        std::string key;
        for (const auto& parameter : function.parameters) {
            if (!key.empty()) key += ";";
            if (parameter.is_const) key += "const ";
            key += canonical_type(parameter.type, &shapes);
            if (parameter.writable) key += "&";
            key += " " + parameter.name;
        }
        return key;
    }

    std::string function_declaration_key(const FunctionDecl& function) const {
        const auto shapes = signature_shape_names(function);
        std::string key = function.name + "(" + generic_call_shape(function) + ")";
        key += "<";
        for (std::size_t i = 0; i < function.type_parameters.size(); ++i) {
            if (i) key += ",";
            key += std::string(function_constraint(function, i));
        }
        key += ">->" + generic_parameter_shape(
            function.return_type, function.type_parameters, &shapes);
        return key;
    }

    void validate_function_family(
        const std::string& name,
        const std::vector<const FunctionDecl*>& family) const {
        const auto arity = family.front()->parameters.size();
        const auto& reference = *family.front();
        std::vector<const FunctionDecl*> generics;
        std::vector<const FunctionDecl*> concretes;

        for (const auto* function : family) {
            if (function->parameters.size() != arity) {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Function '" + name +
                        "' cannot be overloaded by argument count; use default parameters on one definition.",
                    function->span);
            }
            // L7: a specialization may change the implementation, never
            // whether the operation changes existing state.
            if (function->mutation_marker != reference.mutation_marker) {
                frontend_error("MUTATION_MARKER",
                               "Members of a specialization family must agree on the '.' marker: '" +
                                   name + "' is declared '." + name + "' in one member and '" + name +
                                   "' in another.",
                               function->span);
            }
            for (std::size_t i = 0; i < arity; ++i) {
                if (function->parameters[i].name != reference.parameters[i].name ||
                    function->parameters[i].writable != reference.parameters[i].writable ||
                    function->parameters[i].is_const != reference.parameters[i].is_const ||
                    function->parameters[i].is_extension != reference.parameters[i].is_extension ||
                    static_cast<bool>(function->parameters[i].default_value) !=
                        static_cast<bool>(reference.parameters[i].default_value) ||
                    function->is_private != reference.is_private) {
                    frontend_error(
                        "DUPLICATE_NAME",
                        "Repeated function name '" + name +
                            "' is only allowed for one generic specialization family with the same call shape.",
                        function->span);
                }
            }
            if (function->type_parameters.empty()) concretes.push_back(function);
            else generics.push_back(function);
        }

        if (generics.empty()) {
            frontend_error(
                "DUPLICATE_NAME",
                "Function name '" + name +
                    "' is duplicated; ordinary function overloading is not supported.",
                family[1]->span);
        }

        for (std::size_t i = 0; i < generics.size(); ++i) {
            for (std::size_t j = i + 1; j < generics.size(); ++j) {
                if (generics[i]->type_parameters.size() != generics[j]->type_parameters.size() ||
                    (generic_call_shape(*generics[i]) != generic_call_shape(*generics[j]) &&
                     !generic_domains_disjoint(*generics[i], *generics[j]))) {
                    frontend_error(
                        "DUPLICATE_NAME",
                        "Generic specializations of '" + name +
                            "' must keep one generic call shape or have disjoint tensor/array domains; ordinary overloads are not supported.",
                        generics[j]->span);
                }
            }
        }

        for (const auto* function : concretes) {
            if (!std::any_of(generics.begin(), generics.end(),
                    [&](const FunctionDecl* generic) {
                        return concrete_matches_generic_pattern(*generic, *function);
                    }) &&
                !std::all_of(generics.begin(), generics.end(),
                    [&](const FunctionDecl* generic) {
                        return concrete_outside_generic_primitive_domain(*generic, *function);
                    })) {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Concrete specialization of '" + name +
                        "' must instantiate the generic family's parameter type pattern.",
                    function->span);
            }
        }

        std::unordered_set<std::string> concrete_signatures;
        for (const auto* function : concretes) {
            if (!concrete_signatures.insert(concrete_signature_key(*function)).second) {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Concrete specialization of '" + name + "' is duplicated.",
                    function->span);
            }
        }

        for (std::size_t i = 0; i < generics.size(); ++i) {
            for (std::size_t j = i + 1; j < generics.size(); ++j) {
                if (generic_more_specific(*generics[i], *generics[j]) ||
                    generic_more_specific(*generics[j], *generics[i]) ||
                    generic_domains_disjoint(*generics[i], *generics[j])) {
                    continue;
                }
                frontend_error(
                    "AMBIGUOUS_SPECIALIZATION",
                    "Generic specializations of '" + name +
                        "' have overlapping type domains with no unique priority.",
                    generics[j]->span);
            }
        }
    }

    void validate_declarations() const {
        std::unordered_set<std::string> declaration_names;
        for (const auto& enum_decl : source_.enums) {
            if (enum_decl.name == "main" || is_language_type_name(enum_decl.name) ||
                is_reserved_value_name(enum_decl.name) ||
                !declaration_names.insert(enum_decl.name).second) {
                frontend_error("DUPLICATE_NAME",
                               "Enum name '" + enum_decl.name + "' is reserved or duplicated.",
                               enum_decl.span);
            }
            std::unordered_set<std::string> variants;
            for (const auto& variant : enum_decl.variants) {
                if (is_reserved_value_name(variant.name) || !variants.insert(variant.name).second)
                    frontend_error("DUPLICATE_NAME",
                                   "Enum variant '" + variant.name + "' is reserved or duplicated.",
                                   variant.span);
            }
        }
        for (const auto& class_decl : source_.classes) {
            if (class_decl.name == "main") {
                frontend_error("RESERVED_MAIN",
                               "'main' is reserved for the compiler-generated native entrypoint.",
                               class_decl.span);
            }
            if (is_language_type_name(class_decl.name) || is_reserved_value_name(class_decl.name) ||
                !declaration_names.insert(class_decl.name).second) {
                frontend_error("DUPLICATE_NAME",
                               "Class name '" + class_decl.name + "' is reserved or duplicated.",
                               class_decl.span);
            }
        }
        std::unordered_map<std::string, std::vector<const FunctionDecl*>> function_families;
        std::unordered_set<std::string> function_names;
        for (const auto& function : source_.functions) {
            if (function.name == "main") {
                frontend_error("RESERVED_MAIN",
                               "'main' is reserved for the compiler-generated native entrypoint.",
                               function.span);
            }
            if (is_language_type_name(function.name) || is_reserved_value_name(function.name)) {
                frontend_error("DUPLICATE_NAME",
                               "Function name '" + function.name + "' is reserved or duplicated.",
                               function.span);
            }
            if (function_names.insert(function.name).second &&
                !declaration_names.insert(function.name).second) {
                frontend_error("DUPLICATE_NAME",
                               "Function name '" + function.name +
                                   "' conflicts with another top-level declaration.",
                               function.span);
            }
            function_families[function.name].push_back(&function);
        }

        for (const auto& class_decl : source_.classes) {
            validate_type_parameters(
                class_decl.type_parameters, class_decl.type_constraints,
                declaration_names, class_decl.module_namespace, class_decl.span);
            const std::unordered_set<std::string> class_parameters{
                class_decl.type_parameters.begin(), class_decl.type_parameters.end()};
            // Imported package classes are namespace-scoped. Their members are
            // never introduced as bare names in the importing program, so a
            // package can preserve natural qualified member APIs
            // without weakening reservation for root user declarations.
            const bool namespace_scoped =
                class_decl.standard_library ||
                class_decl.name.find('.') != std::string::npos;
            if (!class_decl.self_name.empty() &&
                (is_reserved_value_name(class_decl.self_name) || declaration_names.contains(class_decl.self_name) ||
                 class_parameters.contains(class_decl.self_name)))
                frontend_error("SHADOWING", "The instance name conflicts with an existing declaration or reserved name.", class_decl.self_span.value_or(class_decl.span));
            std::unordered_set<std::string> member_names;
            for (const auto& field : class_decl.fields) {
                if (!namespace_scoped && is_reserved_value_name(field.name)) {
                    frontend_error("SHADOWING",
                                   "Class field name '" + field.name + "' is reserved.",
                                   field.span);
                }
                if (!member_names.insert(field.name).second) {
                    frontend_error("DUPLICATE_NAME",
                                   "Duplicate class member '" + field.name + "'.",
                                   field.span);
                }
            }
            std::unordered_map<std::string, std::vector<const FunctionDecl*>>
                method_families;
            bool constructor_seen = false;
            for (const auto& method : class_decl.methods) {
                if (method.is_constructor) {
                    if (constructor_seen) {
                        frontend_error(
                            "DUPLICATE_NAME",
                            "Class '" + class_decl.name +
                                "' has more than one generation initializer.",
                            method.span);
                    }
                    constructor_seen = true;
                    validate_constructor(class_decl, method);
                    continue;
                }
                if (!namespace_scoped && is_reserved_value_name(method.name)) {
                    frontend_error("SHADOWING",
                                   "Class method name '" + method.name + "' is reserved.",
                                   method.span);
                }
                if (member_names.contains(method.name) &&
                    !method_families.contains(method.name)) {
                    frontend_error(
                        "DUPLICATE_NAME",
                        "Class method '" + method.name +
                            "' conflicts with another class member.",
                        method.span);
                }
                member_names.insert(method.name);
                method_families[method.name].push_back(&method);
                validate_type_parameters(
                    method.type_parameters, method.type_constraints,
                    class_parameters, std::string{}, method.span);
            }
            for (const auto& [name, family] : method_families) {
                if (family.size() > 1) validate_function_family(name, family);
            }
        }

        for (const auto& function : source_.functions) {
            validate_type_parameters(
                function.type_parameters, function.type_constraints,
                declaration_names, function.module_namespace, function.span);
        }
        for (const auto& [name, family] : function_families) {
            if (family.size() > 1) validate_function_family(name, family);
        }
    }

    Program source_;
    Program output_;
    std::unordered_map<std::string, ClassDecl*> class_templates_;
    std::unordered_map<std::string, ClassDecl*> concrete_classes_;
    std::unordered_map<std::string, std::vector<FunctionDecl*>> function_families_;
    std::unordered_map<std::string, std::vector<FunctionDecl*>> specialization_families_;
    std::unordered_map<std::string, FunctionDecl*> function_templates_;
    std::unordered_map<std::string, FunctionDecl*> concrete_functions_;

    std::unordered_map<std::string, std::string> class_instances_;
    std::unordered_map<std::string, TypeName> class_instance_types_;
    std::unordered_map<std::string, std::string> function_instances_;
    std::unordered_map<std::string, std::string> concrete_specialization_instances_;
    std::unordered_map<std::string, std::size_t> class_index_;

    std::unordered_map<std::string, std::unordered_map<std::string, FunctionDecl>> generic_methods_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::vector<MethodSpecializationMember>>>
        method_specialization_families_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::string>>
        method_specialization_instances_;
    std::unordered_map<std::string, std::unordered_set<std::string>> instantiated_methods_;
    std::unordered_map<std::string, TypeName> function_return_types_;
    std::unordered_map<std::string, std::unordered_map<std::string, TypeName>> method_return_types_;
    std::unordered_map<std::string, TypeName> type_environment_;
    std::unordered_map<const Expr*, TypeName> comprehension_types_;
    std::optional<TypeName> traversal_expected_result_;
    std::string current_module_namespace_;
    std::unordered_map<std::string, std::unordered_set<std::string>> extension_visible_modules_;

    struct ModuleScope {
        std::string& current;
        std::string previous;
        ModuleScope(std::string& target, const std::string& scope)
            : current(target), previous(std::exchange(target, scope)) {}
        ~ModuleScope() { current = std::move(previous); }
    };

    void build_extension_scopes() {
        std::unordered_map<std::string, std::vector<const ImportDecl*>> children;
        for (const auto& import : source_.imports) {
            const auto separator = import.alias.rfind('.');
            const auto parent = separator == std::string::npos
                ? std::string{} : import.alias.substr(0, separator);
            children[parent].push_back(&import);
        }
        for (const auto& [parent, imports] : children) {
            auto& visible = extension_visible_modules_[parent];
            const auto add_public = [&](const auto& self, const std::string& module) -> void {
                if (!visible.insert(module).second) return;
                if (const auto descendants = children.find(module); descendants != children.end()) {
                    for (const auto* child : descendants->second)
                        if (child->is_public) self(self, child->alias);
                }
            };
            for (const auto* import : imports) add_public(add_public, import->alias);
        }
    }

    bool extension_visible(const FunctionDecl& declaration) const {
        if (declaration.module_namespace == current_module_namespace_) return true;
        const auto scope = extension_visible_modules_.find(current_module_namespace_);
        return scope != extension_visible_modules_.end() &&
               scope->second.contains(declaration.module_namespace);
    }

    bool receiver_has_method(const TypeName& receiver, const std::string& method) const {
        const auto named = [&](std::initializer_list<std::string_view> names) {
            return std::find(names.begin(), names.end(), method) != names.end();
        };
        if (!receiver.dimensions.empty()) {
            if (named({"indices", "append", "concat"})) return true;
            if (receiver.dimensions.size() != 1) return false;
            if (method == "join") return receiver.name == "string";
            const auto* numeric = numeric_type_name(receiver.name);
            return method == "sorted" &&
                ((numeric && numeric->sort_kind.has_value()) ||
                 receiver.name == "bool" || receiver.name == "string");
        }
        if (receiver.name == "tensor") {
            if (named({"track", "untrack", "retrack", "backward", "clear_grad",
                       "transpose", "contiguous", "reshape", "gather", "scatter",
                       "shape", "device", "cpu", "gpu", "is_contiguous",
                       "is_tracked", "has_grad", "item"})) return true;
            return named({"all", "any"}) && !receiver.arguments.empty() &&
                   receiver.arguments.front().name == "bool";
        }
        if (receiver.name == "string")
            return named({"string", "contains", "starts_with", "ends_with", "find",
                          "trim", "split", "utf8", "codepoints"});
        if (receiver.name == standard_class::autograd_target)
            return named({"has_grad", "clear_grad", "gradient"});
        if (receiver.name == standard_class::file_handle)
            return named({"read", "read_line", "read_bin", "write", "write_line",
                          "flush", "seek", "close"});
        if (method == "string" &&
            (numeric_type_name(receiver.name) || receiver.name == "bool" ||
             receiver.name == "bin" || receiver.name == "error"))
            return true;
        auto class_name = receiver.name;
        if (const auto instance = class_instance_types_.find(class_name);
            instance != class_instance_types_.end()) class_name = instance->second.name;
        for (const auto& cls : source_.classes) {
            if (cls.name != class_name) continue;
            return std::any_of(cls.methods.begin(), cls.methods.end(),
                [&](const FunctionDecl& candidate) {
                    return !candidate.is_constructor && candidate.name == method;
                });
        }
        return false;
    }

    static std::unordered_set<std::string> set_of(const std::vector<std::string>& values) {
        return {values.begin(), values.end()};
    }

    std::string instance_key(const std::string& name, const std::vector<TypeName>& arguments) const {
        std::string key = name + "<";
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i) key += ",";
            key += canonical_type(arguments[i]);
        }
        return key + ">";
    }

    std::string mangle(std::string_view kind, const std::string& name, const std::string& key) const {
        return abi::generic_instance_prefix(kind, name) + short_hash(key);
    }

    bool class_has_generic_method(const std::string& class_name, const std::string& method) const {
        const auto it = generic_methods_.find(class_name);
        return it != generic_methods_.end() && it->second.contains(method);
    }

    std::optional<std::string> generic_method_owner(
        const std::string& class_name,
        const std::string& method) const {
        return class_has_generic_method(class_name, method)
            ? std::optional<std::string>{class_name}
            : std::nullopt;
    }

    bool class_has_method_specialization_family(
        const std::string& class_name,
        const std::string& method) const {
        const auto classes = method_specialization_families_.find(class_name);
        return classes != method_specialization_families_.end() &&
            classes->second.contains(method);
    }

    Substitution clone_substitution(const Substitution& source) const {
        Substitution result;
        for (const auto& [name, type] : source) {
            result.emplace(name, clone_type(type));
        }
        return result;
    }

    MethodSpecializationMember make_method_specialization_member(
        const FunctionDecl& source,
        const Substitution& class_substitution) {
        MethodSpecializationMember member;
        member.source = &source;
        member.class_substitution = clone_substitution(class_substitution);

        auto& signature = member.signature;
        signature.name = source.name;
        signature.source_file = source.source_file;
        signature.module_namespace = source.module_namespace;
        signature.span = source.span;
        signature.is_private = source.is_private;
        signature.mutation_marker = source.mutation_marker;
        signature.type_parameters = source.type_parameters;
        signature.type_constraints = source.type_constraints;
        const auto deferred = set_of(source.type_parameters);
        signature.return_type =
            materialize_type(source.return_type, class_substitution, deferred,true);
        for (const auto& parameter : source.parameters) {
            Parameter copy;
            copy.name = parameter.name;
            copy.type =
                materialize_type(parameter.type, class_substitution, deferred);
            copy.writable = parameter.writable;
            copy.span = parameter.span;
            copy.is_const = parameter.is_const;
            copy.is_local = parameter.is_local;
            if (parameter.default_value) {
                copy.default_value = standard_name("$method_family_default");
            }
            signature.parameters.push_back(std::move(copy));
        }
        return member;
    }

    const ClassDecl* output_class(const std::string& name) const {
        const auto index = class_index_.find(name);
        if (index == class_index_.end()) return nullptr;
        return &output_.classes[index->second];
    }

    const FieldDecl* output_field(const std::string& class_name, const std::string& field) const {
        const auto* declaration = output_class(class_name);
        if (!declaration) return nullptr;
        const auto it = std::find_if(
            declaration->fields.begin(), declaration->fields.end(),
            [&](const auto& candidate) { return candidate.name == field; });
        return it == declaration->fields.end() ? nullptr : &*it;
    }

    const FunctionDecl* output_method(const std::string& class_name, const std::string& method) const {
        const auto* declaration = output_class(class_name);
        if (!declaration) return nullptr;
        const auto it = std::find_if(
            declaration->methods.begin(), declaration->methods.end(),
            [&](const auto& candidate) { return candidate.name == method; });
        return it == declaration->methods.end() ? nullptr : &*it;
    }

    std::optional<TypeName> method_return_type(
        const std::string& class_name,
        const std::string& method) const {
        const auto methods = method_return_types_.find(class_name);
        if (methods == method_return_types_.end()) return std::nullopt;
        const auto result = methods->second.find(method);
        return result == methods->second.end()
            ? std::nullopt
            : std::optional<TypeName>{clone_type(result->second)};
    }

    std::optional<TypeName> infer_expression_type(
        const Expr& expression,
        const std::string& current_class) const {
        const auto simple_type = [&](std::string name) {
            TypeName result;
            result.name = std::move(name);
            result.span = expression.span;
            return std::optional<TypeName>{std::move(result)};
        };
        if (expression.contextual_default_type &&
            expression.contextual_default_type->name != "auto") {
            auto result = clone_type(*expression.contextual_default_type);
            result.span = expression.span;
            return result;
        }
        // Numeric literals carry only a family until a surrounding concrete type
        // determines their representation. Generic inference must not invent
        // default int/float types for them.
        if (std::holds_alternative<IntegerExpr>(expression.data) ||
            std::holds_alternative<RealLiteralExpr>(expression.data) ||
            std::holds_alternative<LiteralConstantExpr>(expression.data)) {
            return std::nullopt;
        }
        if (std::holds_alternative<StringExpr>(expression.data) ||
            std::holds_alternative<StringTemplateExpr>(expression.data)) return simple_type("string");
        if (std::holds_alternative<BoolExpr>(expression.data)) return simple_type("bool");

        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (name->self_reference && !current_class.empty()) return simple_type(current_class);
            if (const auto local = type_environment_.find(name->name);
                !name->this_qualifier && !name->self_reference && local != type_environment_.end()) {
                return clone_type(local->second);
            }
            if (name->this_qualifier && !current_class.empty()) {
                if (const auto* field = output_field(current_class, name->name)) {
                    return clone_type(field->type);
                }
            }
            return std::nullopt;
        }

        if(const auto* tuple=std::get_if<TupleExpr>(&expression.data)) {
            TypeName type;type.name="$tuple";type.span=expression.span;
            for(const auto& element:tuple->elements) {
                auto inferred=infer_expression_type(*element,current_class);
                if(!inferred) return std::nullopt;
                type.arguments.push_back(std::move(*inferred));
            }
            return type;
        }

        if (std::holds_alternative<ArrayComprehensionExpr>(expression.data)) {
            if (const auto type = comprehension_types_.find(&expression); type != comprehension_types_.end())
                return clone_type(type->second);
            return std::nullopt;
        }
        if (const auto* array = std::get_if<ArrayExpr>(&expression.data)) {
            if (array->elements.empty()) return std::nullopt;
            const auto first = infer_expression_type(*array->elements.front(), current_class);
            if (!first) return std::nullopt;
            for (std::size_t i = 1; i < array->elements.size(); ++i) {
                const auto current =
                    infer_expression_type(*array->elements[i], current_class);
                if (!current || canonical_type(*current) != canonical_type(*first)) {
                    return std::nullopt;
                }
            }
            auto result = clone_type(*first);
            result.dimensions.insert(
                result.dimensions.begin(),
                static_cast<long long>(array->elements.size()));
            result.array_depth = result.dimensions.size();
            result.span = expression.span;
            return result;
        }

        if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
            if (unary->op == "not") return simple_type("bool");
            return infer_expression_type(*unary->operand, current_class);
        }

        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            const bool relational = is_relational_operator(binary->op);
            const bool equality = binary->op == "==" || binary->op == "!=";
            if (!relational && !equality && binary->op != "+" && binary->op != "-" &&
                binary->op != "*" && binary->op != "/" &&
                binary->op != "%" && binary->op != "^") {
                return std::nullopt;
            }
            auto left = infer_expression_type(*binary->left, current_class);
            auto right = infer_expression_type(*binary->right, current_class);
            const auto numeric_literal = [](const Expr& value) {
                return std::holds_alternative<IntegerExpr>(value.data) ||
                       std::holds_alternative<RealLiteralExpr>(value.data);
            };
            const auto with_span = [&](TypeName value) {
                value.span = expression.span;
                if (relational || equality) {
                    if (value.name == "tensor" && value.dimensions.empty()) {
                        value.arguments.clear();
                        value.arguments.push_back(*simple_type("bool"));
                    } else if (equality) {
                        return simple_type("bool");
                    } else {
                        value.name = "bool";
                        value.arguments.clear();
                    }
                }
                return std::optional<TypeName>{std::move(value)};
            };
            if (left && right) {
                if (canonical_type(*left) == canonical_type(*right))
                    return with_span(clone_type(*left));
                const auto tensor_scalar = [&](const TypeName& tensor,
                                               const TypeName& scalar)
                    -> std::optional<TypeName> {
                    if (tensor.name != "tensor" ||
                        tensor.arguments.size() != 1 ||
                        canonical_type(tensor.arguments.front()) !=
                            canonical_type(scalar)) {
                        return std::nullopt;
                    }
                    return with_span(clone_type(tensor));
                };
                if (const auto result = tensor_scalar(*left, *right))
                    return result;
                if (const auto result = tensor_scalar(*right, *left))
                    return result;
                const auto array_scalar = [&](const TypeName& array,
                                              const TypeName& scalar)
                    -> std::optional<TypeName> {
                    if (array.dimensions.empty() || !scalar.dimensions.empty())
                        return std::nullopt;
                    auto element = clone_type(array);
                    element.dimensions.clear();
                    element.array_depth = 0;
                    if (canonical_type(element) != canonical_type(scalar))
                        return std::nullopt;
                    return with_span(clone_type(array));
                };
                if (const auto result = array_scalar(*left, *right)) return result;
                if (const auto result = array_scalar(*right, *left)) return result;
                if (!left->dimensions.empty() &&
                    left->dimensions.size() == right->dimensions.size()) {
                    auto left_element = clone_type(*left);
                    auto right_element = clone_type(*right);
                    left_element.dimensions.clear();
                    right_element.dimensions.clear();
                    left_element.array_depth = right_element.array_depth = 0;
                    if (canonical_type(left_element) == canonical_type(right_element)) {
                        auto result = clone_type(*left);
                        for (std::size_t axis = 0; axis < result.dimensions.size(); ++axis) {
                            if (result.dimensions[axis] < 0)
                                result.dimensions[axis] = right->dimensions[axis];
                            else if (right->dimensions[axis] >= 0 &&
                                     result.dimensions[axis] != right->dimensions[axis])
                                return std::nullopt;
                        }
                        return with_span(std::move(result));
                    }
                }
                return std::nullopt;
            }
            if (left && numeric_literal(*binary->right))
                return with_span(clone_type(*left));
            if (right && numeric_literal(*binary->left))
                return with_span(clone_type(*right));
            if (left && !left->dimensions.empty() &&
                std::holds_alternative<ArrayExpr>(binary->right->data))
                return with_span(clone_type(*left));
            if (right && !right->dimensions.empty() &&
                std::holds_alternative<ArrayExpr>(binary->left->data))
                return with_span(clone_type(*right));
            if (relational && std::holds_alternative<ArrayExpr>(binary->left->data) &&
                std::holds_alternative<ArrayExpr>(binary->right->data)) {
                // Literal-array comparisons have a known bool result shape
                // even before the checker contextualizes their numeric leaves.
                const auto literal_shape = [&](const auto& self, const Expr& value)
                    -> std::optional<std::vector<long long>> {
                    const auto* array = std::get_if<ArrayExpr>(&value.data);
                    if (!array) return std::vector<long long>{};
                    std::vector<long long> shape;
                    bool first = true;
                    for (const auto& element : array->elements) {
                        const auto inner = self(self, *element);
                        if (!inner) return std::nullopt;
                        if (first) { shape = *inner; first = false; }
                        else {
                            if (shape.size() != inner->size()) return std::nullopt;
                            for (std::size_t axis = 0; axis < shape.size(); ++axis)
                                if (shape[axis] != (*inner)[axis]) shape[axis] = -1;
                        }
                    }
                    shape.insert(shape.begin(), static_cast<long long>(array->elements.size()));
                    return shape;
                };
                const auto left_shape = literal_shape(literal_shape, *binary->left);
                const auto right_shape = literal_shape(literal_shape, *binary->right);
                if (left_shape && right_shape && left_shape->size() == right_shape->size()) {
                    auto result = *simple_type("bool");
                    result.dimensions = *left_shape;
                    result.array_depth = result.dimensions.size();
                    return result;
                }
            }
            return std::nullopt;
        }

        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            if (call->callee == "$std.range.generate") {
                auto inferred = infer_expression_type(*call->args[0].value, current_class);
                if (!inferred)
                    inferred = infer_expression_type(*call->args[1].value, current_class);
                // simple_type() returns an optional TypeName; value_or()
                // requires a concrete TypeName rather than another optional.
                TypeName result;
                if (inferred) result = clone_type(*inferred);
                else result.name = "int";
                result.dimensions.insert(result.dimensions.begin(), -1);
                result.array_depth = result.dimensions.size();
                result.span = expression.span;
                return result;
            }
            if ((call->callee == "tensor" || call->callee == "$std.tensor.zeros" ||
                 call->callee == "$std.tensor.ones") &&
                call->type_arguments.size() == 1 && call->args.size() == 1) {
                TypeName result;
                result.name = "tensor";
                result.arguments.push_back(clone_type(call->type_arguments.front()));
                const auto shape =
                    infer_expression_type(*call->args[0].value, current_class);
                if (shape && names_shape_element(shape->name) &&
                    shape->dimensions.size() == 1 &&
                    shape->dimensions.front() >= 0) {
                    result.tensor_rank = shape->dimensions.front();
                }
                result.span = expression.span;
                return result;
            }
            if (const auto* numeric = numeric_type_name(call->callee)) {
                return simple_type(std::string(numeric->spelling));
            }
            if (class_index_.contains(call->callee)) {
                TypeName result;
                result.name = call->callee;
                result.span = expression.span;
                return result;
            }
            if (const auto function = function_return_types_.find(call->callee);
                function != function_return_types_.end()) {
                return clone_type(function->second);
            }
            if (!current_class.empty()) {
                if (const auto result = method_return_type(current_class, call->callee)) return result;
            }
            return std::nullopt;
        }

        if (const auto* member = std::get_if<MemberExpr>(&expression.data)) {
            const auto base = infer_expression_type(*member->base, current_class);
            if (!base || !base->dimensions.empty()) return std::nullopt;
            if (base->name == "tensor" && member->name == "grad") {
                auto result = clone_type(*base);
                result.span = expression.span;
                return result;
            }
            if (const auto* field = output_field(base->name, member->name)) {
                return clone_type(field->type);
            }
            return std::nullopt;
        }

        if (const auto* index = std::get_if<IndexExpr>(&expression.data)) {
            auto base = infer_expression_type(*index->base, current_class);
            if (!base) return std::nullopt;
            if (base->name == "tensor" && base->dimensions.empty()) {
                if (index->items.size() == 1 && !index->items.front().slice &&
                    index->items.front().index) {
                    const auto selector = infer_expression_type(
                        *index->items.front().index, current_class);
                    if (selector && selector->name == "tensor") {
                        base->tensor_rank = 1;
                        base->tensor_shape_prefix.clear();
                        base->tensor_known_shape_prefix.clear();
                        base->tensor_shape_expressions.clear();
                        base->tensor_shape_ellipsis.reset();
                        base->shape_axes.clear();
                        base->brace_shape = false;
                        return base;
                    }
                }
                if (base->tensor_rank) {
                    if (index->items.size() > static_cast<std::size_t>(*base->tensor_rank)) {
                        return std::nullopt;
                    }
                    long long rank = *base->tensor_rank;
                    for (const auto& item : index->items) {
                        if (!item.slice) --rank;
                    }
                    base->tensor_rank = rank;
                }
                if (base->tensor_shape_ellipsis) {
                    // Leading constraints are usable flow facts. A dynamic
                    // middle has no fixed position to project trailing axes
                    // through, so indexing does not retain that source shape.
                    const auto leading = *base->tensor_shape_ellipsis;
                    if (base->tensor_known_shape_prefix.size() < leading)
                        base->tensor_known_shape_prefix.resize(leading);
                    for (std::size_t axis = 0; axis < leading; ++axis)
                        if (base->tensor_shape_prefix[axis] >= 0)
                            base->tensor_known_shape_prefix[axis] = base->tensor_shape_prefix[axis];
                    base->tensor_shape_prefix.clear();
                    base->tensor_shape_expressions.clear();
                    base->tensor_shape_ellipsis.reset();
                    base->shape_axes.clear();
                    base->brace_shape = false;
                }
                const auto project_prefix = [&](auto& prefix) {
                    std::size_t axis = 0;
                    for (const auto& item : index->items) {
                        if (!item.slice) {
                            if (axis < prefix.size()) prefix.erase(prefix.begin() + axis);
                        } else {
                            ++axis;
                        }
                    }
                };
                project_prefix(base->tensor_shape_prefix);
                project_prefix(base->tensor_known_shape_prefix);
                return base;
            }
            if (index->items.size() != 1) return std::nullopt;
            if (base->dimensions.empty()) {
                if (base->name == "bin" ||
                    (base->name == "string" && index->items.front().slice)) return base;
                return std::nullopt;
            }
            const auto& item = index->items.front();
            const auto selector = item.index
                ? infer_expression_type(*item.index, current_class) : std::nullopt;
            if (item.slice || (selector && !selector->dimensions.empty()) ||
                (item.index && std::holds_alternative<ArrayExpr>(item.index->data))) {
                // Slices and gather/mask selectors retain the element type,
                // with a runtime outer extent, matching the checker.
                base->dimensions.front() = -1;
                if (!base->dimension_expressions.empty())
                    base->dimension_expressions.front().reset();
                return base;
            }
            base->dimensions.erase(base->dimensions.begin());
            if (!base->dimension_expressions.empty())
                base->dimension_expressions.erase(base->dimension_expressions.begin());
            base->array_depth = base->dimensions.size();
            return base;
        }

        if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
            const auto receiver = infer_expression_type(*call->receiver, current_class);
            if (!receiver) return std::nullopt;
            if (!receiver->dimensions.empty()) {
                if (call->method == "indices") {
                    auto result = *simple_type("nat");
                    result.dimensions = {-1};
                    result.array_depth = 1;
                    return result;
                }
                if (call->method == "append" || call->method == "concat" ||
                    call->method == "sorted") {
                    auto result = clone_type(*receiver);
                    result.dimensions.front() = -1;
                    if (!result.dimension_expressions.empty())
                        result.dimension_expressions.front().reset();
                    return result;
                }
                if (call->method == "join" && receiver->name == "string" &&
                    receiver->dimensions.size() == 1) return simple_type("string");
                return std::nullopt;
            }
            if (receiver->name == "tensor") {
                if ((call->method == "cpu" && call->args.empty()) ||
                    (call->method == "gpu" && call->args.size() == 1)) {
                    return receiver;
                }
                if (call->method == "device" && call->args.empty()) {
                    TypeName result;
                    result.name = "int";
                    result.span = expression.span;
                    return result;
                }
                if ((call->method == "contiguous" || call->method == "track" ||
                     call->method == "untrack" || call->method == "retrack") &&
                    call->args.empty()) {
                    return receiver;
                }
                if (call->method == "shape" && call->args.empty()) {
                    TypeName result;
                    result.name = "nat";
                    result.dimensions.push_back(receiver->tensor_rank.value_or(-1));
                    result.array_depth = 1;
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "reshape" && call->args.size() == 1) {
                    auto result = clone_type(*receiver);
                    result.tensor_shape_prefix.clear();
                    result.tensor_shape_ellipsis.reset();
                    result.tensor_shape_expressions.clear();
                    result.shape_axes.clear();
                    result.brace_shape = false;
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[0].value, current_class);
                    if (shape && names_shape_element(shape->name) &&
                        shape->dimensions.size() == 1 &&
                        shape->dimensions.front() >= 0) {
                        result.tensor_rank = shape->dimensions.front();
                    }
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "transpose" && call->args.size() == 2) {
                    auto result = clone_type(*receiver);
                    // Transpose preserves tensor dtype and rank. Axis values may
                    // not be statically available during specialization-family
                    // selection, so discard extent facts rather than returning
                    // an unknown type or inventing a permuted shape.
                    result.tensor_shape_prefix.clear();
                    result.tensor_shape_ellipsis.reset();
                    result.tensor_shape_expressions.clear();
                    result.shape_axes.clear();
                    result.brace_shape = false;
                    result.tensor_known_shape_prefix.clear();
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "gather" && call->args.size() == 2) {
                    auto result = clone_type(*receiver);
                    result.tensor_shape_prefix.clear();
                    result.tensor_shape_ellipsis.reset();
                    result.tensor_shape_expressions.clear();
                    result.shape_axes.clear();
                    result.brace_shape = false;
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[1].value, current_class);
                    if (shape && names_shape_element(shape->name) &&
                        shape->dimensions.size() == 1 &&
                        shape->dimensions.front() >= 0) {
                        result.tensor_rank = shape->dimensions.front();
                    }
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "scatter" && call->args.size() == 2) {
                    auto result = clone_type(*receiver);
                    result.tensor_shape_prefix.clear();
                    result.tensor_shape_ellipsis.reset();
                    result.tensor_shape_expressions.clear();
                    result.shape_axes.clear();
                    result.brace_shape = false;
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[1].value, current_class);
                    if (shape && names_shape_element(shape->name) &&
                        shape->dimensions.size() == 1 &&
                        shape->dimensions.front() >= 0) {
                        result.tensor_rank = shape->dimensions.front();
                    }
                    result.span = expression.span;
                    return result;
                }
                if ((call->method == "is_tracked" || call->method == "has_grad") &&
                    call->args.empty()) {
                    TypeName result;
                    result.name = "bool";
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "clear_grad" && call->args.empty()) {
                    TypeName result;
                    result.name = "void";
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "item" && call->args.empty() &&
                    receiver->arguments.size() == 1) {
                    return clone_type(receiver->arguments.front());
                }
            }
            if (const auto result = method_return_type(receiver->name, call->method)) return result;
            return std::nullopt;
        }

        if (const auto* try_expr = std::get_if<TryExpr>(&expression.data)) {
            return infer_expression_type(*try_expr->value, current_class);
        }

        if (const auto* node = std::get_if<IfExpr>(&expression.data)) {
            // The one type of the branches; a numeric literal branch takes it.
            std::optional<TypeName> result;
            const auto consider = [&](const Expr& value) {
                const Expr* literal = &value;
                if (const auto* unary = std::get_if<UnaryExpr>(&value.data);
                    unary && unary->op == "-") {
                    literal = unary->operand.get();
                }
                if (std::holds_alternative<IntegerExpr>(literal->data) ||
                    std::holds_alternative<RealLiteralExpr>(literal->data)) {
                    return true;
                }
                auto current = infer_expression_type(value, current_class);
                if (!current) return false;
                if (result) return canonical_type(*current) == canonical_type(*result);
                result = std::move(current);
                return true;
            };
            for (const auto& value : node->values) {
                if (!consider(*value)) return std::nullopt;
            }
            if (!consider(*node->otherwise)) return std::nullopt;
            if (result) result->span = expression.span;
            return result;
        }

        return std::nullopt;
    }

    bool infer_generic_pattern(
        const TypeName& pattern,
        const TypeName& actual,
        const std::unordered_set<std::string>& parameters,
        Substitution& inferred) const {
        if (parameters.contains(pattern.name) && pattern.arguments.empty()) {
            if (actual.dimensions.size() < pattern.dimensions.size()) return false;
            for (std::size_t i = 0; i < pattern.dimensions.size(); ++i) {
                const auto required = pattern.dimensions[i];
                const auto observed = actual.dimensions[i];
                if (required >= 0 && required != observed) return false;
            }

            auto candidate = clone_type(actual);
            candidate.dimensions.erase(
                candidate.dimensions.begin(),
                candidate.dimensions.begin() +
                    static_cast<std::ptrdiff_t>(pattern.dimensions.size()));
            candidate.dimension_expressions.erase(
                candidate.dimension_expressions.begin(),
                candidate.dimension_expressions.begin() + static_cast<std::ptrdiff_t>(
                    std::min(pattern.dimensions.size(), candidate.dimension_expressions.size())));
            candidate.array_depth = candidate.dimensions.size();

            if (const auto existing = inferred.find(pattern.name);
                existing != inferred.end()) {
                return canonical_type(existing->second) == canonical_type(candidate);
            }
            inferred.emplace(pattern.name, std::move(candidate));
            return true;
        }

        // Recover only a class instance registered from checked type arguments.
        // A single generic parameter above still binds the concrete instance.
        if (const auto origin = class_instance_types_.find(actual.name);
            origin != class_instance_types_.end() &&
            canonical_type_name(pattern.name) == canonical_type_name(origin->second.name)) {
            const auto registered = class_instances_.find(
                instance_key(origin->second.name, origin->second.arguments));
            if (registered != class_instances_.end() && registered->second == actual.name) {
                auto original = clone_type(origin->second);
                original.dimensions = actual.dimensions;
                original.array_depth = actual.array_depth;
                original.span = actual.span;
                return infer_generic_pattern(pattern, original, parameters, inferred);
            }
        }

        if (canonical_type_name(pattern.name) != canonical_type_name(actual.name) ||
            pattern.arguments.size() != actual.arguments.size() ||
            pattern.function_parameters.size() != actual.function_parameters.size() ||
            pattern.dimensions.size() != actual.dimensions.size()) {
            return false;
        }
        if (pattern.name == "tensor" &&
            !pattern.tensor_shape_prefix.empty()) {
            const auto shape_type = [](const TypeName& name) {
                return Type::tensor(Type::simple(TypeKind::Invalid),
                    name.tensor_rank.value_or(-1), name.tensor_shape_prefix,
                    name.tensor_known_shape_prefix, name.tensor_shape_ellipsis);
            };
            if (!tensor_satisfies_shape_prefix(shape_type(actual), shape_type(pattern))) return false;
        }
        for (std::size_t i = 0; i < pattern.dimensions.size(); ++i) {
            const auto required = pattern.dimensions[i];
            if (required >= 0 && required != actual.dimensions[i]) return false;
        }
        for (std::size_t i = 0; i < pattern.arguments.size(); ++i) {
            if (!infer_generic_pattern(
                    pattern.arguments[i], actual.arguments[i], parameters, inferred)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < pattern.function_parameters.size(); ++i) {
            if (!infer_generic_pattern(
                    pattern.function_parameters[i], actual.function_parameters[i],
                    parameters, inferred)) {
                return false;
            }
        }
        return true;
    }

    std::optional<std::vector<TypeName>> infer_generic_arguments(
        const FunctionDecl& templ,
        const std::vector<CallArg>& args,
        const std::string& current_class) const {
        return infer_type_arguments(
            templ.type_parameters, templ.type_parameters, templ.parameters, args, current_class,&templ.return_type);
    }

    // Solves `solved` (the type parameters the call's arguments may bind)
    // against the parameter types, and returns the arguments of `wanted`, a
    // subset of `solved`, in order; nullopt when an argument does not match
    // or a wanted parameter stays undetermined.
    std::optional<std::vector<TypeName>> infer_type_arguments(
        const std::vector<std::string>& solved,
        const std::vector<std::string>& wanted,
        const std::vector<Parameter>& callee_parameters,
        const std::vector<CallArg>& args,
        const std::string& current_class,const TypeName* result_pattern=nullptr) const {
        const std::unordered_set<std::string> parameters{solved.begin(), solved.end()};
        Substitution inferred;
        std::vector<bool> filled(callee_parameters.size(), false);
        std::size_t positional = 0;

        for (const auto& argument : args) {
            std::size_t index = callee_parameters.size();
            if (argument.name) {
                for (std::size_t i = 0; i < callee_parameters.size(); ++i) {
                    if (callee_parameters[i].name == *argument.name) {
                        index = i;
                        break;
                    }
                }
            } else {
                while (positional < filled.size() && filled[positional]) ++positional;
                index = positional++;
            }
            if (index >= callee_parameters.size() || filled[index]) return std::nullopt;
            filled[index] = true;

            const auto actual = infer_expression_type(*argument.value, current_class);
            if (!actual) continue;
            if (!infer_generic_pattern(
                    callee_parameters[index].type, *actual, parameters, inferred)) {
                return std::nullopt;
            }
        }

        if(result_pattern&&result_pattern->name=="$std.reflect.Each"&&traversal_expected_result_) {
            if(!infer_generic_pattern(*result_pattern,*traversal_expected_result_,parameters,inferred))return std::nullopt;
        }
        std::vector<TypeName> result;
        result.reserve(wanted.size());
        for (const auto& parameter : wanted) {
            const auto found = inferred.find(parameter);
            if (found == inferred.end()) return std::nullopt;
            result.push_back(clone_type(found->second));
        }
        return result;
    }

    // `Box(...)` of a generic class without type arguments: the class's type
    // arguments follow from its constructor's parameter types against the
    // call's arguments, with the rules of generic functions. A constructor
    // that declares type parameters of its own takes part in the same solve;
    // its own arguments are selected afterwards on the instance. A class
    // without a constructor has nothing to infer from (nullptr result).
    const FunctionDecl* class_template_constructor(const ClassDecl& templ) const {
        for (const auto& method : templ.methods) {
            if (method.is_constructor) return &method;
        }
        return nullptr;
    }

    std::optional<std::vector<TypeName>> infer_class_arguments(
        const ClassDecl& templ,
        const FunctionDecl& constructor,
        const std::vector<CallArg>& args,
        const std::string& current_class) const {
        auto solved = templ.type_parameters;
        solved.insert(solved.end(), constructor.type_parameters.begin(),
                      constructor.type_parameters.end());
        return infer_type_arguments(
            solved, templ.type_parameters, constructor.parameters, args, current_class);
    }

    std::optional<std::vector<std::size_t>> bind_family_arguments(
        const FunctionDecl& function, const std::vector<CallArg>& args) const {
        std::vector<std::size_t> targets;
        std::vector<bool> filled(function.parameters.size());
        std::size_t positional = 0;
        for (const auto& argument : args) {
            std::size_t target = function.parameters.size();
            if (argument.name) {
                for (std::size_t i = 0; i < function.parameters.size(); ++i) {
                    if (function.parameters[i].name == *argument.name) {
                        target = i;
                        break;
                    }
                }
            } else if (positional < function.parameters.size()) {
                target = positional++;
            }
            if (target >= function.parameters.size() || filled[target]) return std::nullopt;
            if (argument.writable != function.parameters[target].writable) return std::nullopt;
            filled[target] = true;
            targets.push_back(target);
        }
        for (std::size_t i = 0; i < filled.size(); ++i) {
            if (!filled[i] && !function.parameters[i].default_value) return std::nullopt;
        }
        return targets;
    }

    bool generic_arguments_satisfy(
        const FunctionDecl& function,
        const std::vector<TypeName>& arguments) const {
        if (arguments.size() != function.type_parameters.size()) return false;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto constraint = function_constraint(function, i);
            if (!constraint.empty() &&
                !satisfies_generic_constraint(clone_type(arguments[i]), constraint)) {
                return false;
            }
        }
        return true;
    }

    static std::optional<TypeName> non_error_type(const TypeName& type) {
        if (type.name != "union" || !type.dimensions.empty()) return std::nullopt;
        auto residual = clone_type(type);
        std::erase_if(residual.arguments, [](const TypeName& member) {
            return member.name == "error" && member.dimensions.empty();
        });
        if (residual.arguments.empty() || residual.arguments.size() == type.arguments.size())
            return std::nullopt;
        if (residual.arguments.size() == 1) return clone_type(residual.arguments.front());
        return residual;
    }

    bool concrete_argument_type_match(
        const TypeName& expected, const TypeName& actual, bool allow_error_unwrap) const {
        Substitution ignored;
        if (infer_generic_pattern(expected, actual, {}, ignored)) return true;
        // Ordinary value arguments may fail fast on an error alternative when
        // their concrete destination accepts the success type. References and
        // inferred generic arguments keep their original declared type.
        if (allow_error_unwrap) {
            if (const auto residual = non_error_type(actual))
                return infer_generic_pattern(expected, *residual, {}, ignored);
        }
        return false;
    }

    bool concrete_family_match(
        const FunctionDecl& function,
        const std::vector<CallArg>& args,
        const std::string& current_class,
        const Substitution* explicit_arguments = nullptr,
        bool allow_contextual_error_unwrap = false) const {
        const auto targets = bind_family_arguments(function, args);
        if (!targets) return false;
        const auto contextual_match = [&](const auto& self, const Expr& value,
                                          const TypeName& expected, bool allow_error_unwrap) -> bool {
            if (const auto actual = infer_expression_type(value, current_class)) {
                return concrete_argument_type_match(expected, *actual, allow_error_unwrap);
            }
            if (const auto* array = std::get_if<ArrayExpr>(&value.data)) {
                if (expected.dimensions.empty()) return false;
                if (expected.dimensions.front() >= 0 &&
                    expected.dimensions.front() != static_cast<long long>(array->elements.size()))
                    return false;
                auto element = clone_type(expected);
                element.dimensions.erase(element.dimensions.begin());
                if (!element.dimension_expressions.empty())
                    element.dimension_expressions.erase(element.dimension_expressions.begin());
                element.array_depth = element.dimensions.size();
                return std::all_of(array->elements.begin(), array->elements.end(),
                    [&](const auto& item) { return self(self, *item, element, allow_error_unwrap); });
            }
            if (const auto* conditional = std::get_if<IfExpr>(&value.data)) {
                return self(self, *conditional->otherwise, expected, allow_error_unwrap) &&
                    std::all_of(conditional->values.begin(), conditional->values.end(),
                        [&](const auto& item) { return self(self, *item, expected, allow_error_unwrap); });
            }
            if (const auto* unary = std::get_if<UnaryExpr>(&value.data)) {
                return (unary->op == "-" || unary->op == "+") &&
                    self(self, *unary->operand, expected, allow_error_unwrap);
            }
            if (const auto* binary = std::get_if<BinaryExpr>(&value.data)) {
                return expected.dimensions.empty() &&
                    (binary->op == "+" || binary->op == "-" || binary->op == "*" ||
                     binary->op == "/" || binary->op == "%" || binary->op == "^") &&
                    self(self, *binary->left, expected, allow_error_unwrap) &&
                    self(self, *binary->right, expected, allow_error_unwrap);
            }
            if (const auto* literal = std::get_if<LiteralConstantExpr>(&value.data))
                return self(self, *literal->value, expected, allow_error_unwrap);
            return (std::holds_alternative<IntegerExpr>(value.data) ||
                    std::holds_alternative<RealLiteralExpr>(value.data)) &&
                expected.dimensions.empty() && expected.arguments.empty() &&
                expected.function_parameters.empty() &&
                scalar_numeric_constraint_type(expected.name);
        };
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& pattern = function.parameters[(*targets)[i]].type;
            std::optional<TypeName> substituted;
            const auto& expected = explicit_arguments
                ? substituted.emplace(substitute_raw(pattern, *explicit_arguments)) : pattern;
            if (!contextual_match(contextual_match, *args[i].value, expected,
                                  allow_contextual_error_unwrap &&
                                  !function.parameters[(*targets)[i]].writable)) return false;
        }
        return true;
    }

    std::optional<std::vector<TypeName>> generic_family_match(
        const FunctionDecl& function,
        const std::vector<CallArg>& args,
        const std::string& current_class) const {
        if (!bind_family_arguments(function, args)) return std::nullopt;
        const auto inferred = infer_generic_arguments(function, args, current_class);
        if (!inferred || !generic_arguments_satisfy(function, *inferred)) {
            return std::nullopt;
        }
        return inferred;
    }

    const FunctionDecl* most_specific_generic(
        const std::vector<const FunctionDecl*>& candidates,
        SourceSpan span, const std::string& name) const {
        if (candidates.empty()) return nullptr;
        if (candidates.size() == 1) return candidates.front();
        for (const auto* candidate : candidates) {
            bool dominates = true;
            for (const auto* other : candidates) {
                if (candidate == other) continue;
                if (!generic_more_specific(*candidate, *other)) {
                    dominates = false;
                    break;
                }
            }
            if (dominates) return candidate;
        }
        frontend_error(
            "AMBIGUOUS_SPECIALIZATION",
            "Call to '" + name +
                "' matches multiple generic specializations with no unique priority.",
            span);
    }

    std::string materialize_concrete_specialization(const FunctionDecl& function) {
        const auto key = function_declaration_key(function);
        if (const auto existing = concrete_specialization_instances_.find(key);
            existing != concrete_specialization_instances_.end()) {
            return existing->second;
        }
        const auto concrete_name = mangle("fs", function.name, key);
        concrete_specialization_instances_[key] = concrete_name;
        function_return_types_[concrete_name] =
            materialize_type(function.return_type, {}, {},true);
        auto concrete = clone_function(function, {}, {}, "");
        concrete.name = concrete_name;
        output_.functions.push_back(std::move(concrete));
        return concrete_name;
    }

    std::string resolve_function_family(
        const std::string& name,
        const std::vector<CallArg>& args,
        const std::string& current_class,
        SourceSpan span) {
        const auto family = specialization_families_.find(name);
        if (family == specialization_families_.end()) {
            frontend_error("UNKNOWN_GENERIC", "Unknown specialization family '" + name + "'.", span);
        }

        std::vector<const FunctionDecl*> concrete_matches;
        struct GenericMatch {
            const FunctionDecl* function{};
            std::vector<TypeName> arguments;
        };
        std::vector<GenericMatch> generic_matches;

        for (const auto* function : family->second) {
            if (function->type_parameters.empty()) {
                if (concrete_family_match(*function, args, current_class)) {
                    concrete_matches.push_back(function);
                }
            } else if (const auto inferred =
                           generic_family_match(*function, args, current_class)) {
                generic_matches.push_back(
                    GenericMatch{function, *inferred});
            }
        }

        if (concrete_matches.size() > 1) {
            frontend_error(
                "AMBIGUOUS_SPECIALIZATION",
                "Call to '" + name + "' matches more than one concrete specialization.",
                span);
        }
        if (concrete_matches.size() == 1) {
            return materialize_concrete_specialization(*concrete_matches.front());
        }
        if (generic_matches.empty()) {
            frontend_error(
                "SPECIALIZATION_NO_MATCH",
                "No specialization of '" + name +
                    "' matches the statically known argument types.",
                span);
        }

        std::vector<const FunctionDecl*> candidates;
        for (const auto& match : generic_matches) candidates.push_back(match.function);
        const auto* chosen = most_specific_generic(candidates, span, name);
        for (const auto& match : generic_matches) {
            if (match.function == chosen) {
                return instantiate_function(*chosen, match.arguments);
            }
        }
        throw std::logic_error("Generic specialization selection lost its candidate.");
    }

    enum class ContainerDomain { Scalar, Array, Tensor };

    std::optional<ContainerDomain> expression_container_domain(
        const Expr& expression, const std::string& current_class) const {
        auto actual = infer_expression_type(expression, current_class);
        if (actual) {
            if (auto residual = non_error_type(*actual)) actual = std::move(residual);
        }
        if (actual && !actual->dimensions.empty()) return ContainerDomain::Array;
        if (actual && actual->name == "tensor") return ContainerDomain::Tensor;
        if (actual && actual->name == "union") return std::nullopt;
        if (actual) return ContainerDomain::Scalar;
        if (std::holds_alternative<IntegerExpr>(expression.data) ||
            std::holds_alternative<RealLiteralExpr>(expression.data))
            return ContainerDomain::Scalar;
        // The outer domain can be known while an element still needs contextual
        // typing from the explicit type argument. Every conditional branch must
        // agree; an unknown or mixed branch remains unknown.
        if (std::holds_alternative<ArrayExpr>(expression.data) ||
            std::holds_alternative<ArrayComprehensionExpr>(expression.data))
            return ContainerDomain::Array;
        if (const auto* call = std::get_if<CallExpr>(&expression.data);
            call && call->callee == "$std.range.generate")
            return ContainerDomain::Array;
        if (const auto* call = std::get_if<CallExpr>(&expression.data);
            call && (call->callee == "tensor" || call->callee == "$std.tensor.zeros" ||
                     call->callee == "$std.tensor.ones")) return ContainerDomain::Tensor;
        if (const auto* conditional = std::get_if<IfExpr>(&expression.data)) {
            const auto domain = expression_container_domain(*conditional->otherwise, current_class);
            if (!domain) return std::nullopt;
            for (const auto& value : conditional->values)
                if (expression_container_domain(*value, current_class) != domain) return std::nullopt;
            return domain;
        }
        return std::nullopt;
    }

    void filter_explicit_container_domains(
        std::vector<const FunctionDecl*>& candidates,
        const std::vector<CallArg>& args,
        const std::string& current_class) const {
        if (candidates.empty()) return;
        const auto shape = generic_call_shape(*candidates.front());
        const bool different_domains = std::any_of(candidates.begin(), candidates.end(),
            [&](const FunctionDecl* function) { return generic_call_shape(*function) != shape; });
        if (different_domains) {
            std::vector<std::optional<ContainerDomain>> domains;
            for (const auto& argument : args) {
                domains.push_back(expression_container_domain(*argument.value, current_class));
            }
            std::erase_if(candidates, [&](const FunctionDecl* function) {
                const auto targets = bind_family_arguments(*function, args);
                if (!targets) return false; // The ordinary call checker diagnoses this.
                for (std::size_t i = 0; i < args.size(); ++i) {
                    const auto& pattern = function->parameters[(*targets)[i]].type;
                    bool scalar_pattern = pattern.dimensions.empty() && pattern.name != "tensor";
                    if (const auto parameter = std::find(function->type_parameters.begin(),
                                                         function->type_parameters.end(), pattern.name);
                        scalar_pattern && parameter != function->type_parameters.end()) {
                        const auto constraint = function_constraint(*function,
                            static_cast<std::size_t>(parameter - function->type_parameters.begin()));
                        scalar_pattern = constraint == "numeric" || constraint == "ordered" ||
                                         constraint == "integer" || constraint == "floating";
                    }
                    if ((domains[i] == ContainerDomain::Array && pattern.name == "tensor" &&
                         pattern.dimensions.empty()) ||
                        (domains[i] == ContainerDomain::Tensor && !pattern.dimensions.empty()) ||
                        ((domains[i] == ContainerDomain::Array || domains[i] == ContainerDomain::Tensor) &&
                         scalar_pattern) ||
                        (domains[i] == ContainerDomain::Scalar &&
                         (!pattern.dimensions.empty() || pattern.name == "tensor")))
                        return true;
                }
                return false;
            });
        }
    }

    std::string resolve_function_family_explicit(
        const std::string& name,
        const std::vector<TypeName>& source_arguments,
        const std::vector<CallArg>& args,
        const std::string& current_class,
        SourceSpan span) {
        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) {
            arguments.push_back(materialize_type(argument, {}, {}));
        }
        std::vector<const FunctionDecl*> candidates;
        for (const auto* function : specialization_families_.at(name)) {
            if (function->type_parameters.empty()) continue;
            if (function->type_parameters.size() != arguments.size()) continue;
            if (generic_arguments_satisfy(*function, arguments)) {
                candidates.push_back(function);
            }
        }
        if (candidates.empty()) {
            frontend_error(
                "GENERIC_CONSTRAINT",
                "Explicit type arguments do not match any generic specialization of '" +
                    name + "'.",
                span);
        }
        filter_explicit_container_domains(candidates, args, current_class);
        if (candidates.empty()) {
            frontend_error("SPECIALIZATION_NO_MATCH",
                "No specialization of '" + name + "' matches the statically known argument types.",
                span);
        }
        const auto* chosen = most_specific_generic(candidates, span, name);
        return instantiate_function(*chosen, arguments);
    }

    TypeName materialize_type(
        const TypeName& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred,bool traversal_result=false) {
        auto type = substitute_raw(source, substitution);

        if(type.name=="$std.reflect.Each"&&(!traversal_result||!type.dimensions.empty()))
            frontend_error("TRAVERSAL","reflect.Each<E> is only a traversal function's complete result type.",type.span);
        if (type.name == "union" || type.name == "$tuple" || type.name == "$std.reflect.Each") {
            for (auto& argument : type.arguments) {
                argument = materialize_type(argument, {}, deferred);
            }
            return type;
        }

        if (deferred.contains(type.name) && type.arguments.empty()) return type;

        for (auto& argument : type.arguments) {
            argument = materialize_type(argument, {}, deferred);
        }
        for (auto& parameter : type.function_parameters) {
            parameter = materialize_type(parameter, {}, deferred);
        }

        if (!type.arguments.empty()) {
            if (contains_parameter(type, deferred)) return type;
            if (type.name == "tensor") {
                if (type.arguments.size() != 1) {
                    frontend_error("GENERIC_ARITY", "tensor requires exactly one element type.", type.span);
                }
                return type;
            }
            if (type.name == "func") {
                if (type.arguments.size() != 1) {
                    frontend_error("GENERIC_ARITY", "fn requires exactly one result type.", type.span);
                }
                return type;
            }
            if (class_templates_.contains(type.name)) {
                const auto dimensions = type.dimensions;
                const auto span = type.span;
                const auto concrete = instantiate_class(type.name, type.arguments);
                TypeName result;
                result.name = concrete;
                result.dimensions = dimensions;
                result.array_depth = dimensions.size();
                result.span = span;
                return result;
            }
            if (concrete_classes_.contains(type.name) || class_index_.contains(type.name)) {
                frontend_error("GENERIC_ARITY",
                               "Non-generic class '" + type.name + "' does not accept type arguments.",
                               type.span);
            }
            frontend_error("UNKNOWN_GENERIC", "Unknown generic class '" + type.name + "'.", type.span);
        }

        if (class_templates_.contains(type.name)) {
            frontend_error("GENERIC_ARGUMENTS_REQUIRED",
                           "Generic class '" + type.name + "' requires explicit type arguments.",
                           type.span);
        }

        return type;
    }

    std::vector<TypeName> materialize_type_arguments(
        const std::vector<TypeName>& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred) {
        std::vector<TypeName> result;
        for (const auto& argument : source) {
            result.push_back(materialize_type(argument, substitution, deferred));
        }
        return result;
    }

    std::vector<CallArg> clone_args(
        const std::vector<CallArg>& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred,
        const std::string& current_class) {
        std::vector<CallArg> result;
        for (const auto& argument : source) {
            CallArg copy;
            copy.name = argument.name;
            copy.writable = argument.writable;
            copy.span = argument.span;
            copy.value = clone_expr(*argument.value, substitution, deferred, current_class);
            result.push_back(std::move(copy));
        }
        return result;
    }

    static bool is_relational_operator(const std::string& op) {
        return op == "<" || op == "<=" || op == ">" || op == ">=";
    }

    // L14: `a < b < c > (d)` reads as `a < b<c>(d)`, a generic call of `b`,
    // because a type-argument list is tried before the comparison. When `b`
    // names no function, class or generic declaration it is a value, so the
    // source is a chain whose last '>' turns its direction, which is
    // CHAINED_COMPARISON at the '<' before the callee rather than a
    // generic-call error.
    void reject_misread_generic_call(
        const Expr& operand, const SourceSpan& op_span, const std::string& current_class) const {
        const auto* call = std::get_if<CallExpr>(&operand.data);
        if (!call || call->type_arguments.empty()) return;
        const auto& name = call->callee;
        if (name == "tensor" || name.starts_with("$std.") || class_templates_.contains(name) ||
            concrete_classes_.contains(name) || function_templates_.contains(name) ||
            concrete_functions_.contains(name) || function_families_.contains(name) ||
            specialization_families_.contains(name) ||
            (!current_class.empty() &&
             (class_has_generic_method(current_class, name) ||
              class_has_method_specialization_family(current_class, name))))
            return;
        std::string hint = "parenthesize the comparison that '>' starts";
        if (call->type_arguments.size() == 1 && call->args.size() == 1 &&
            call->type_arguments[0].arguments.empty() &&
            call->type_arguments[0].array_depth == 0 && !call->args[0].name &&
            !call->args[0].writable) {
            const auto& value = *call->args[0].value;
            std::optional<std::string> spelled;
            if (const auto* argument = std::get_if<NameExpr>(&value.data);
                argument && !argument->this_qualifier && !argument->self_reference)
                spelled = argument->name;
            else if (const auto* literal = std::get_if<IntegerExpr>(&value.data))
                spelled = literal->spelling.empty() ? std::to_string(literal->value)
                                                    : literal->spelling;
            if (spelled)
                hint = "write '(" + call->type_arguments[0].name + " > " + *spelled +
                       ")' if '>' starts a new comparison";
        }
        frontend_error("CHAINED_COMPARISON",
                       "A comparison chain must keep one direction: '<' and '<=', or '>' and "
                       "'>=', or only '=='; " + hint + ".",
                       op_span.end.offset > op_span.start.offset ? op_span : operand.span);
    }

    ExprPtr clone_expr(
        const Expr& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred,
        const std::string& current_class) {
        nesting::DepthGuard guard(
            clone_depth_, nesting::max_ast_clone_depth, source.span, "Expression");
        auto out = std::make_unique<Expr>();
        out->span = source.span;
        if (source.contextual_default_type) {
            out->contextual_default_type = clone_type(*source.contextual_default_type);
        }
        out->constant_name = source.constant_name;

        if (const auto* node = std::get_if<IntegerExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<RealLiteralExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<ImaginaryLiteralExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<LiteralConstantExpr>(&source.data))
            out->data = LiteralConstantExpr{
                node->name, clone_expr(*node->value, substitution, deferred, current_class), node->origin};
        else if (const auto* node = std::get_if<StringExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<BoolExpr>(&source.data)) out->data = *node;
        else if (std::holds_alternative<VoidExpr>(source.data)) out->data = VoidExpr{};
    else if (std::holds_alternative<NoneExpr>(source.data)) out->data = NoneExpr{};
        else if (const auto* node = std::get_if<NameExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<StringTemplateExpr>(&source.data)) {
            StringTemplateExpr copy;
            copy.literals = node->literals;
            copy.formats = node->formats;
            for (const auto& item : node->expressions) {
                copy.expressions.push_back(clone_expr(*item, substitution, deferred, current_class));
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<TupleExpr>(&source.data)) {
            TupleExpr copy;
            for (const auto& item : node->elements) {
                copy.elements.push_back(clone_expr(*item, substitution, deferred, current_class));
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<ArrayComprehensionExpr>(&source.data)) {
            comprehension_types_.erase(out.get());
            ArrayComprehensionExpr copy;
            copy.name = node->name;
            copy.binding_span = node->binding_span;
            copy.source = clone_expr(*node->source, substitution, deferred, current_class);
            if (node->type) copy.type = materialize_type(*node->type, substitution, deferred);
            const auto before = type_environment_;
            if (copy.type) type_environment_[copy.name] = clone_type(*copy.type);
            else if (auto source_type = infer_expression_type(*copy.source, current_class);
                     source_type && !source_type->dimensions.empty()) {
                source_type->dimensions.erase(source_type->dimensions.begin());
                source_type->array_depth = source_type->dimensions.size();
                type_environment_[copy.name] = std::move(*source_type);
            }
            try {
                if (node->condition) copy.condition = clone_expr(*node->condition, substitution, deferred, current_class);
                copy.value = clone_expr(*node->value, substitution, deferred, current_class);
                if (auto result = infer_expression_type(*copy.value, current_class)) {
                    result->dimensions.insert(result->dimensions.begin(), -1);
                    result->array_depth = result->dimensions.size();
                    comprehension_types_[out.get()] = std::move(*result);
                }
            } catch (...) { type_environment_ = before; throw; }
            type_environment_ = before;
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<ArrayExpr>(&source.data)) {
            ArrayExpr copy;
            for (const auto& item : node->elements) {
                copy.elements.push_back(clone_expr(*item, substitution, deferred, current_class));
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<IndexExpr>(&source.data)) {
            IndexExpr copy;
            copy.base = clone_expr(*node->base, substitution, deferred, current_class);
            for (const auto& item : node->items) {
                IndexPart cloned;
                cloned.slice = item.slice;
                cloned.start_form = item.start_form;
                cloned.end_form = item.end_form;
                cloned.span = item.span;
                if (item.index) cloned.index = clone_expr(*item.index, substitution, deferred, current_class);
                if (item.start) cloned.start = clone_expr(*item.start, substitution, deferred, current_class);
                if (item.stop) cloned.stop = clone_expr(*item.stop, substitution, deferred, current_class);
                if (item.step) cloned.step = clone_expr(*item.step, substitution, deferred, current_class);
                copy.items.push_back(std::move(cloned));
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<MemberExpr>(&source.data)) {
            out->data = MemberExpr{
                clone_expr(*node->base, substitution, deferred, current_class), node->name};
        } else if (const auto* node = std::get_if<UnaryExpr>(&source.data)) {
            out->data = UnaryExpr{
                node->op, clone_expr(*node->operand, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<BinaryExpr>(&source.data)) {
            if (is_relational_operator(node->op))
                reject_misread_generic_call(*node->right, node->op_span, current_class);
            out->data = BinaryExpr{
                node->op,
                clone_expr(*node->left, substitution, deferred, current_class),
                clone_expr(*node->right, substitution, deferred, current_class),
                node->op_span};
        } else if (const auto* node = std::get_if<ComparisonChainExpr>(&source.data)) {
            ComparisonChainExpr copy;
            for (std::size_t i = 0; i < node->operands.size(); ++i) {
                if (i && is_relational_operator(node->ops[i - 1]))
                    reject_misread_generic_call(
                        *node->operands[i], node->op_spans[i - 1], current_class);
                copy.operands.push_back(
                    clone_expr(*node->operands[i], substitution, deferred, current_class));
            }
            copy.ops = node->ops;
            copy.op_spans = node->op_spans;
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<TryExpr>(&source.data)) {
            out->data = TryExpr{clone_expr(*node->value, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<IfExpr>(&source.data)) {
            IfExpr copy;
            for (const auto& condition : node->conditions)
                copy.conditions.push_back(clone_expr(*condition, substitution, deferred, current_class));
            for (const auto& value : node->values)
                copy.values.push_back(clone_expr(*value, substitution, deferred, current_class));
            copy.otherwise = clone_expr(*node->otherwise, substitution, deferred, current_class);
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<CallExpr>(&source.data)) {
            auto type_arguments = materialize_type_arguments(node->type_arguments, substitution, deferred);
            const bool deferred_call = std::any_of(
                type_arguments.begin(), type_arguments.end(),
                [&](const auto& argument) { return contains_parameter(argument, deferred); });

            CallExpr copy;
            copy.callee = node->callee;
            // A numeric type parameter in call position denotes the explicit
            // conversion of its concrete specialization, including tensors.
            if (const auto target = substitution.find(copy.callee);
                target != substitution.end() && target->second.dimensions.empty() &&
                target->second.arguments.empty() && target->second.function_parameters.empty() &&
                numeric_type_name(target->second.name)) {
                copy.callee = target->second.name;
            }
            copy.constructor = node->constructor;
            copy.mutation_marker = node->mutation_marker;
            copy.args = clone_args(node->args, substitution, deferred, current_class);

            if (!type_arguments.empty() && !deferred_call) {
                if (copy.callee == "tensor" || copy.callee == "$std.tensor.zeros" ||
                    copy.callee == "$std.tensor.ones" ||
                    copy.callee == "$std.reflect.collect" ||
                    copy.callee == "$std.reflect.paths" || copy.callee == "$std.reflect.each" || copy.callee == "$std.reflect.each_paths") {
                    copy.type_arguments = std::move(type_arguments);
                } else if (class_templates_.contains(copy.callee)) {
                    copy.callee = instantiate_class(copy.callee, type_arguments);
                } else if (current_class.size() &&
                           class_has_method_specialization_family(
                               current_class, copy.callee)) {
                    copy.callee = resolve_method_family_explicit(
                        current_class, copy.callee, type_arguments, copy.args, current_class, source.span);
                } else if (current_class.size() && class_has_generic_method(current_class, copy.callee)) {
                    request_method_for_class(current_class, copy.callee, type_arguments, source.span);
                    copy.callee = method_name(copy.callee, type_arguments);
                } else if (specialization_families_.contains(copy.callee)) {
                    copy.callee = resolve_function_family_explicit(
                        copy.callee, type_arguments, copy.args, current_class, source.span);
                } else if (function_templates_.contains(copy.callee)) {
                    copy.callee = instantiate_function(copy.callee, type_arguments);
                } else {
                    frontend_error("GENERIC_TARGET",
                                   "Target '" + copy.callee + "' is not generic.",
                                   source.span);
                }
            } else if (!type_arguments.empty()) {
                copy.type_arguments = std::move(type_arguments);
            } else {
                if (current_class.size() &&
                    class_has_method_specialization_family(
                        current_class, copy.callee)) {
                    copy.callee = resolve_method_family(
                        current_class, copy.callee, copy.args,
                        current_class, source.span);
                } else if (specialization_families_.contains(copy.callee)) {
                    copy.callee = resolve_function_family(
                        copy.callee, copy.args, current_class, source.span);
                } else if (const auto function = function_templates_.find(copy.callee);
                           function != function_templates_.end()) {
                    const auto inferred =
                        infer_generic_arguments(*function->second, copy.args, current_class);
                    if (!inferred) {
                        frontend_error(
                            "GENERIC_INFERENCE",
                            "Generic function '" + copy.callee +
                                "' cannot infer every type argument from this call; provide explicit type arguments.",
                            source.span);
                    }
                    const bool inferred_deferred = std::any_of(
                        inferred->begin(), inferred->end(),
                        [&](const auto& argument) {
                            return contains_parameter(argument, deferred);
                        });
                    if (!inferred_deferred) {
                        copy.callee = instantiate_function(copy.callee, *inferred);
                    }
                } else if (current_class.size() &&
                           class_has_generic_method(current_class, copy.callee)) {
                    const auto owner = generic_method_owner(current_class, copy.callee);
                    const auto& templ = generic_methods_.at(*owner).at(copy.callee);
                    const auto inferred =
                        infer_generic_arguments(templ, copy.args, current_class);
                    if (!inferred) {
                        frontend_error(
                            "GENERIC_INFERENCE",
                            "Generic method '" + copy.callee +
                                "' cannot infer every type argument from this call; provide explicit type arguments.",
                            source.span);
                    }
                    const bool inferred_deferred = std::any_of(
                        inferred->begin(), inferred->end(),
                        [&](const auto& argument) {
                            return contains_parameter(argument, deferred);
                        });
                    if (!inferred_deferred) {
                        request_method_for_class(
                            current_class, copy.callee, *inferred, source.span);
                        copy.callee = method_name(copy.callee, *inferred);
                    }
                } else if (const auto templ = class_templates_.find(copy.callee);
                           templ != class_templates_.end()) {
                    const auto* constructor = class_template_constructor(*templ->second);
                    if (!constructor) {
                        frontend_error(
                            "GENERIC_ARGUMENTS_REQUIRED",
                            "Generic class '" + copy.callee +
                                "' requires explicit type arguments.",
                            source.span);
                    }
                    const auto inferred = infer_class_arguments(
                        *templ->second, *constructor, copy.args, current_class);
                    if (!inferred) {
                        std::string parameters;
                        for (const auto& parameter : templ->second->type_parameters) {
                            if (!parameters.empty()) parameters += ", ";
                            parameters += parameter;
                        }
                        frontend_error(
                            "GENERIC_INFERENCE",
                            "Class type arguments must follow from the constructor's arguments; write '" +
                                copy.callee + "<" + parameters + ">(...)'.",
                            source.span);
                    }
                    const bool inferred_deferred = std::any_of(
                        inferred->begin(), inferred->end(),
                        [&](const auto& argument) {
                            return contains_parameter(argument, deferred);
                        });
                    if (!inferred_deferred) {
                        copy.callee = instantiate_class(copy.callee, *inferred);
                    }
                }
            }
            if (!deferred_call && copy.type_arguments.empty()) {
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<MethodCallExpr>(&source.data)) {
            // Extension methods are ordinary functions with a statically
            // marked first parameter. Desugar before specialization, so the
            // existing generic-family, borrow and effect mechanisms resolve
            // the same implementation for f(x) and x.f().
            struct ExtensionCandidate {
                std::string name;
                std::string source_file;
                bool writable{};
                bool call_matches{};
            };
            std::vector<ExtensionCandidate> extensions;
            auto receiver = clone_expr(*node->receiver, substitution, deferred, current_class);
            const auto receiver_type = infer_expression_type(*receiver, current_class);
            auto arguments = clone_args(node->args, substitution, deferred, current_class);
            arguments.insert(arguments.begin(), CallArg{
                std::nullopt, false, std::move(receiver), node->receiver->span});
            auto extension_type_arguments =
                materialize_type_arguments(node->type_arguments, substitution, deferred);
            bool contextual_receiver_collision = false;
            for (const auto& [name, family] : function_families_) {
                if (name != node->method &&
                    !(name.size() > node->method.size()+1 &&
                      name.ends_with("." + node->method)))
                    continue;
                if (!extension_visible(*family.front()))
                    continue;
                bool matches = false;
                bool mutable_receiver = false;
                bool call_matches = false;
                bool exact_contextual_match = false;
                bool exact_contextual_collision = false;
                bool fallback_contextual_collision = false;
                for (const auto* declaration : family) {
                    if (declaration->parameters.empty() ||
                        !declaration->parameters.front().is_extension ||
                        (declaration->is_private &&
                         declaration->module_namespace != current_module_namespace_))
                        continue;
                    const auto& param = declaration->parameters.front();
                    if (receiver_type && !contains_parameter(*receiver_type, deferred)) {
                        Substitution inferred;
                        bool receiver_matches = infer_generic_pattern(
                            param.type, *receiver_type, set_of(declaration->type_parameters), inferred);
                        for (std::size_t i = 0; i < declaration->type_parameters.size(); ++i) {
                            const auto argument = inferred.find(declaration->type_parameters[i]);
                            if (argument != inferred.end() &&
                                !satisfies_generic_constraint(argument->second,
                                                              function_constraint(*declaration, i))) {
                                receiver_matches = false;
                                break;
                            }
                        }
                        if (!receiver_matches && !param.writable) {
                            if (declaration->type_parameters.empty()) {
                                receiver_matches = concrete_argument_type_match(
                                    param.type, *receiver_type, true);
                            } else if (!extension_type_arguments.empty() &&
                                       generic_arguments_satisfy(*declaration, extension_type_arguments)) {
                                Substitution explicit_arguments;
                                for (std::size_t i = 0; i < extension_type_arguments.size(); ++i)
                                    explicit_arguments.emplace(declaration->type_parameters[i],
                                                               clone_type(extension_type_arguments[i]));
                                receiver_matches = concrete_argument_type_match(
                                    substitute_raw(param.type, explicit_arguments), *receiver_type, true);
                            }
                        }
                        if (!receiver_matches) continue;
                    }
                    arguments.front().writable = param.writable;
                    bool declaration_matches = false;
                    Substitution candidate_arguments;
                    if (extension_type_arguments.empty()) {
                        if (declaration->type_parameters.empty()) {
                            declaration_matches = concrete_family_match(
                                *declaration, arguments, current_class, nullptr, true);
                        } else if (const auto inferred = generic_family_match(
                                       *declaration, arguments, current_class)) {
                            declaration_matches = true;
                            for (std::size_t i = 0; i < inferred->size(); ++i)
                                candidate_arguments.emplace(declaration->type_parameters[i],
                                                            clone_type((*inferred)[i]));
                        }
                    } else if (generic_arguments_satisfy(*declaration, extension_type_arguments)) {
                        for (std::size_t i = 0; i < extension_type_arguments.size(); ++i)
                            candidate_arguments.emplace(declaration->type_parameters[i],
                                                        clone_type(extension_type_arguments[i]));
                        declaration_matches = concrete_family_match(
                            *declaration, arguments, current_class, &candidate_arguments, true);
                    }
                    // Inference may leave contextual literal arguments untyped.
                    // Check those against the solved parameter types before
                    // counting a generic family as applicable to this call.
                    if (declaration_matches && !declaration->type_parameters.empty())
                        declaration_matches = concrete_family_match(
                            *declaration, arguments, current_class, &candidate_arguments,
                            !extension_type_arguments.empty());
                    // An unknown type is not a wildcard. Literal receivers
                    // can still match through the ordinary family's concrete
                    // contextual argument rules above.
                    if (!receiver_type && !declaration_matches) continue;
                    if (!receiver_type || (declaration_matches && non_error_type(*receiver_type))) {
                        const auto contextual_receiver =
                            materialize_type(param.type, candidate_arguments, deferred);
                        const bool collision = receiver_has_method(contextual_receiver, node->method);
                        if (concrete_family_match(
                                *declaration, arguments, current_class, &candidate_arguments)) {
                            exact_contextual_match = true;
                            exact_contextual_collision |= collision;
                        } else {
                            fallback_contextual_collision |= collision;
                        }
                    }
                    matches = true;
                    mutable_receiver |= param.writable;
                    call_matches |= declaration_matches;
                }
                if (!matches) continue;
                // Inferred dispatch keeps an exact generic union match ahead
                // of a concrete member that only matches after error removal.
                // Such an unused member cannot introduce a native collision.
                contextual_receiver_collision |= exact_contextual_match
                    ? exact_contextual_collision : fallback_contextual_collision;
                extensions.push_back({name, family.front()->source_file,
                                      mutable_receiver, call_matches});
            }
            if (!extensions.empty()) {
                if ((receiver_type && receiver_has_method(*receiver_type, node->method)) ||
                    contextual_receiver_collision)
                    frontend_error("EXTENSION_COLLISION",
                                   "Extension method '" + node->method +
                                   "' conflicts with an existing method of its receiver type.",
                                   source.span);
                if (std::any_of(extensions.begin(), extensions.end(),
                                [](const auto& candidate) { return candidate.call_matches; })) {
                    std::erase_if(extensions,
                                  [](const auto& candidate) { return !candidate.call_matches; });
                }
                // Loading the same source under two import aliases does not
                // create competing providers of the same extension family.
                std::sort(extensions.begin(), extensions.end(),
                    [](const auto& a, const auto& b) { return a.name < b.name; });
                const auto& extension = extensions.front();
                if (std::any_of(extensions.begin() + 1, extensions.end(),
                    [&](const auto& candidate) {
                        return candidate.source_file.empty() ||
                               candidate.source_file != extension.source_file;
                    }))
                    frontend_error("EXTENSION_AMBIGUOUS",
                                   "Extension method '" + node->method +
                                   "' is visible from multiple imported modules; call it with a namespace.",
                                   source.span);
                if (extension.writable && !node->mutation_marker)
                    frontend_error("MUTATION_MARKER",
                                   "An extension with a writable receiver must use '.receiver.method(...)'.",
                                   source.span);
                CallExpr call;
                call.callee = extension.name;
                call.type_arguments = std::move(extension_type_arguments);
                call.mutation_marker = node->mutation_marker;
                arguments.front().writable = extension.writable;
                call.args = std::move(arguments);
                Expr rewritten;
                rewritten.span = source.span;
                rewritten.data = std::move(call);
                return clone_expr(rewritten, {}, deferred, current_class);
            }
            MethodCallExpr copy;
            copy.receiver = std::move(arguments.front().value);
            arguments.erase(arguments.begin());
            copy.method = node->method;
            copy.mutation_marker = node->mutation_marker;
            copy.args = std::move(arguments);

            auto type_arguments = std::move(extension_type_arguments);
            const bool deferred_call = std::any_of(
                type_arguments.begin(), type_arguments.end(),
                [&](const auto& argument) { return contains_parameter(argument, deferred); });
            if (!type_arguments.empty() && !deferred_call && copy.method == "cast") {
                copy.type_arguments = std::move(type_arguments);
            } else if (!type_arguments.empty() && !deferred_call) {
                const auto receiver_type = infer_expression_type(*copy.receiver, current_class);
                if (!receiver_type || !receiver_type->dimensions.empty()) {
                    frontend_error(
                        "GENERIC_RECEIVER",
                        "Generic method receiver type must be statically known during monomorphization.",
                        source.span);
                }
                if (!class_index_.contains(receiver_type->name)) {
                    if (const auto declaration = concrete_classes_.find(receiver_type->name);
                        declaration != concrete_classes_.end()) {
                        materialize_concrete_class(*declaration->second);
                    }
                }
                if (!class_index_.contains(receiver_type->name)) {
                    frontend_error(
                        "GENERIC_RECEIVER",
                        "Generic method receiver must resolve to a concrete class.",
                        source.span);
                }
                if (receiver_type->name == standard_class::autograd_target &&
                    copy.method == "gradient") {
                    // autograd.Target.gradient<T>() is a built-in method. Keep its
                    // explicit type argument for the checker instead of trying to
                    // monomorphize a source-declared generic method.
                    copy.type_arguments = std::move(type_arguments);
                } else if (class_has_method_specialization_family(
                        receiver_type->name, copy.method)) {
                    copy.method = resolve_method_family_explicit(
                        receiver_type->name, copy.method,
                        type_arguments, copy.args, current_class, source.span);
                } else {
                    request_method_for_class(
                        receiver_type->name, copy.method,
                        type_arguments, source.span);
                    copy.method = method_name(copy.method, type_arguments);
                }
            } else if (!type_arguments.empty()) {
                copy.type_arguments = std::move(type_arguments);
            } else {
                const auto receiver_type =
                    infer_expression_type(*copy.receiver, current_class);
                if (receiver_type && receiver_type->dimensions.empty()) {
                    if (!class_index_.contains(receiver_type->name)) {
                        if (const auto declaration = concrete_classes_.find(receiver_type->name);
                            declaration != concrete_classes_.end()) {
                            materialize_concrete_class(*declaration->second);
                        }
                    }
                    if (class_index_.contains(receiver_type->name) &&
                        class_has_method_specialization_family(
                            receiver_type->name, copy.method)) {
                        copy.method = resolve_method_family(
                            receiver_type->name, copy.method, copy.args,
                            current_class, source.span);
                    } else if (class_index_.contains(receiver_type->name) &&
                               class_has_generic_method(receiver_type->name, copy.method)) {
                        const auto owner =
                            generic_method_owner(receiver_type->name, copy.method);
                        const auto& templ =
                            generic_methods_.at(*owner).at(copy.method);
                        const auto inferred =
                            infer_generic_arguments(templ, copy.args, current_class);
                        if (!inferred) {
                            frontend_error(
                                "GENERIC_INFERENCE",
                                "Generic method '" + copy.method +
                                    "' cannot infer every type argument from this call; provide explicit type arguments.",
                                source.span);
                        }
                        const bool inferred_deferred = std::any_of(
                            inferred->begin(), inferred->end(),
                            [&](const auto& argument) {
                                return contains_parameter(argument, deferred);
                            });
                        if (!inferred_deferred) {
                            request_method_for_class(
                                receiver_type->name, copy.method, *inferred, source.span);
                            copy.method = method_name(copy.method, *inferred);
                        }
                    }
                }
            }
            out->data = std::move(copy);
        }
        return out;
    }

    Pattern clone_pattern(const Pattern& source,const Substitution& substitution,
                          const std::unordered_set<std::string>& deferred,const std::string& current_class) {
        Pattern copy;copy.kind=source.kind;copy.name=source.name;copy.reference=source.reference;copy.is_const=source.is_const;copy.span=source.span;
        if(source.type)copy.type=materialize_type(*source.type,substitution,deferred);
        if(source.place)copy.place=clone_expr(*source.place,substitution,deferred,current_class);
        for(const auto& element:source.elements)copy.elements.push_back(clone_pattern(element,substitution,deferred,current_class));
        return copy;
    }

    void infer_pattern_bindings(const Pattern& pattern,const TypeName& subject) {
        if(pattern.kind==Pattern::Kind::Tuple){if(subject.name!="$tuple" || subject.arguments.size()!=pattern.elements.size())return;for(std::size_t i=0;i<pattern.elements.size();++i)infer_pattern_bindings(pattern.elements[i],subject.arguments[i]);return;}
        if(pattern.kind==Pattern::Kind::Declare)type_environment_[pattern.name]=clone_type(pattern.type && pattern.type->name!="auto"?*pattern.type:subject);
    }

    StmtPtr clone_stmt(
        const Stmt& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred,
        const std::string& current_class) {
        nesting::DepthGuard guard(
            clone_depth_, nesting::max_ast_clone_depth, source.span, "Statement");
        auto out = std::make_unique<Stmt>();
        out->span = source.span;

        if (const auto* node = std::get_if<BindingStmt>(&source.data)) {
            BindingStmt copy;
            copy.declared_type = materialize_type(node->declared_type, substitution, deferred);
            copy.name = node->name;
            copy.reference = node->reference;
            copy.reference_initializer = node->reference_initializer;
            copy.is_const = node->is_const;
            copy.literal_constant = node->literal_constant;
            copy.is_local = node->is_local;
            if(node->pattern)copy.pattern=clone_pattern(*node->pattern,substitution,deferred,current_class);
            if (node->value) copy.value = clone_expr(*node->value, substitution, deferred, current_class);
            if(copy.pattern){if(copy.value)if(const auto inferred=infer_expression_type(*copy.value,current_class))infer_pattern_bindings(*copy.pattern,*inferred);}
            else if (copy.declared_type.name != "auto") {
                type_environment_[copy.name] = clone_type(copy.declared_type);
            } else if (copy.value) {
                if (const auto inferred = infer_expression_type(*copy.value, current_class)) {
                    type_environment_[copy.name] = clone_type(*inferred);
                }
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<AssignStmt>(&source.data)) {
            AssignStmt copy;
            if(node->target)copy.target=clone_expr(*node->target,substitution,deferred,current_class);
            copy.value=clone_expr(*node->value,substitution,deferred,current_class);copy.compound_op=node->compound_op;
            if(node->pattern)copy.pattern=clone_pattern(*node->pattern,substitution,deferred,current_class);
            out->data=std::move(copy);
        } else if (const auto* node = std::get_if<RebindStmt>(&source.data)) {
            out->data = RebindStmt{
                node->name, clone_expr(*node->target, substitution, deferred, current_class),
                node->place ? clone_expr(*node->place, substitution, deferred, current_class) : nullptr};
        } else if (const auto* node = std::get_if<ReturnStmt>(&source.data)) {
            out->data = ReturnStmt{
                clone_expr(*node->value, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<LoopControlStmt>(&source.data)) {
            out->data = *node;
        } else if (const auto* node = std::get_if<ExprStmt>(&source.data)) {
            out->data = ExprStmt{
                clone_expr(*node->value, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<IfStmt>(&source.data)) {
            IfStmt copy;
            copy.condition = clone_expr(*node->condition, substitution, deferred, current_class);
            const auto before = type_environment_;
            for (const auto& child : node->then_body) {
                copy.then_body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            }
            type_environment_ = before;
            for (const auto& child : node->else_body) {
                copy.else_body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            }
            type_environment_ = before;
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<MainGuardStmt>(&source.data)) {
            ModuleScope module_scope(current_module_namespace_, node->module_namespace);
            MainGuardStmt copy;
            copy.active = node->active;
            copy.source_file = node->source_file;
            copy.module_namespace = node->module_namespace;
            const auto before = type_environment_;
            for (const auto& child : node->body)
                copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            type_environment_ = before;
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<ParallelStmt>(&source.data)) {
            ParallelStmt copy;
            const auto before=type_environment_;
            for (const auto& child : node->body)
                copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            type_environment_=before;
            out->data=std::move(copy);
        } else if (const auto* node = std::get_if<WhileStmt>(&source.data)) {
            WhileStmt copy;
            copy.condition = clone_expr(*node->condition, substitution, deferred, current_class);
            const auto before = type_environment_;
            for (const auto& child : node->body) {
                copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            }
            type_environment_ = before;
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<ForStmt>(&source.data)) {
            ForStmt copy;
            copy.name = node->name;
            copy.writable = node->writable;
            const auto previous_expected=traversal_expected_result_;
            if(node->type) {
                TypeName each;each.name="$std.reflect.Each";
                each.arguments.push_back(substitute_raw(*node->type,substitution));
                traversal_expected_result_=std::move(each);
            }else traversal_expected_result_.reset();
            copy.iterable = clone_expr(*node->iterable, substitution, deferred, current_class);
            traversal_expected_result_=previous_expected;
            if(node->pattern)copy.pattern=clone_pattern(*node->pattern,substitution,deferred,current_class);
            copy.is_auto=node->is_auto;
            copy.declares=node->declares;
            copy.legacy=node->legacy;
            copy.parallel=node->parallel;
            copy.parallel_span=node->parallel_span;
            copy.index=node->index;
            for(const auto& binding:node->collections) {
                ForCollectionBinding extra;
                extra.name=binding.name;
                extra.iterable=clone_expr(*binding.iterable,substitution,deferred,current_class);
                if(binding.type) extra.type=materialize_type(*binding.type,substitution,deferred);
                extra.is_auto=binding.is_auto;
                extra.declares=binding.declares;
                extra.span=binding.span;
                copy.collections.push_back(std::move(extra));
            }
            if(node->type) copy.type=materialize_type(*node->type,substitution,deferred);
            if(node->range) {
                copy.range.emplace();
                copy.range->start_inclusive=node->range->start_inclusive;
                copy.range->end_inclusive=node->range->end_inclusive;
                copy.range->descending=node->range->descending;
                copy.range->implicit_start_zero=node->range->implicit_start_zero;
                copy.range->implicit_end_zero=node->range->implicit_end_zero;
                copy.range->end=clone_expr(*node->range->end,substitution,deferred,current_class);
                if(node->range->step)
                    copy.range->step=clone_expr(*node->range->step,substitution,deferred,current_class);
            }
            const auto before = type_environment_;
            std::vector<std::optional<TypeName>> collection_types;
            for(const auto& binding:copy.collections)
                collection_types.push_back(infer_expression_type(*binding.iterable,current_class));
            if (const auto iterable_type = infer_expression_type(*copy.iterable, current_class);
                iterable_type && !iterable_type->dimensions.empty()) {
                auto element_type = clone_type(*iterable_type);
                element_type.dimensions.erase(element_type.dimensions.begin());
                element_type.array_depth = element_type.dimensions.size();
                if(copy.pattern)infer_pattern_bindings(*copy.pattern,element_type);else type_environment_[copy.name] = std::move(element_type);
            }
            if(copy.type) type_environment_[copy.name]=clone_type(*copy.type);
            for(std::size_t i=0;i<copy.collections.size();++i) {
                const auto& binding=copy.collections[i];
                if(binding.name=="_") continue;
                if(binding.type) type_environment_[binding.name]=clone_type(*binding.type);
                else if(const auto& type=collection_types[i]; type && !type->dimensions.empty()) {
                    auto element=clone_type(*type);
                    element.dimensions.erase(element.dimensions.begin());
                    element.array_depth=element.dimensions.size();
                    type_environment_[binding.name]=std::move(element);
                }
            }
            if(copy.index) {
                TypeName index_type;
                index_type.name="nat";
                type_environment_[copy.index->name]=std::move(index_type);
            }
            for (const auto& child : node->body) {
                copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            }
            type_environment_ = before;
            out->data = std::move(copy);
        } else {
            const auto& match_node = std::get<MatchStmt>(source.data);
            MatchStmt copy;
            copy.value = clone_expr(*match_node.value, substitution, deferred, current_class);
            const auto before = type_environment_;
            for (const auto& match_case : match_node.cases) {
                type_environment_ = before;
                MatchCase case_copy;
                if(!match_case.pattern)case_copy.type = materialize_type(match_case.type, substitution, deferred);
                case_copy.tag = match_case.tag;
                case_copy.binder = match_case.binder;
                case_copy.span = match_case.span;
                if(match_case.pattern)case_copy.pattern=clone_pattern(*match_case.pattern,substitution,deferred,current_class);
                std::optional<TypeName> selected;
                if(case_copy.pattern){if(const auto subject=infer_expression_type(*copy.value,current_class);subject && subject->name=="$union"){
                    for(const auto& candidate:subject->arguments)if(tuple_pattern_matches(*case_copy.pattern,candidate,[](const TypeName& t)->const std::vector<TypeName>* {return t.name=="$tuple" && t.dimensions.empty()?&t.arguments:nullptr;},[](const TypeName& leaf,const TypeName& t){return canonical_type(leaf)==canonical_type(t);})){if(selected){selected.reset();break;}selected=clone_type(candidate);}
                    if(selected)infer_pattern_bindings(*case_copy.pattern,*selected);
                }}else selected=clone_type(case_copy.type);
                if(selected){if(const auto* name=std::get_if<NameExpr>(&copy.value->data);name && !name->this_qualifier && !name->self_reference)type_environment_[name->name]=clone_type(*selected);if(case_copy.binder)type_environment_[*case_copy.binder]=clone_type(*selected);}
                for (const auto& child : match_case.body) {
                    case_copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
                }
                copy.cases.push_back(std::move(case_copy));
            }
            type_environment_ = before;
            out->data = std::move(copy);
        }
        return out;
    }

    FunctionDecl clone_function(
        const FunctionDecl& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred,
        const std::string& current_class) {
        ModuleScope module_scope(current_module_namespace_, source.module_namespace);
        const auto saved_environment = type_environment_;
        const auto saved_expected=traversal_expected_result_;
        type_environment_.clear();
        try {
            FunctionDecl out;
            out.name = source.name;
            out.source_file = source.source_file;
            out.module_namespace = source.module_namespace;
            out.declaration = source.declaration;
            if (out.declaration && !substitution.empty()) {
                out.declaration->specialization_of = declaration_key(*out.declaration);
                out.declaration->generic_args.clear();
                for (const auto& parameter : out.declaration->generic_parameters) {
                    const auto argument = substitution.find(parameter);
                    out.declaration->generic_args.push_back(argument == substitution.end() ? "" : canonical_type(argument->second));
                }
            }
            out.return_type = materialize_type(source.return_type, substitution, deferred,true);
            traversal_expected_result_=out.return_type.name=="$std.reflect.Each"?std::optional<TypeName>{substitute_raw(source.return_type,substitution)}:std::nullopt;
            out.span = source.span;
            out.is_private = source.is_private;
            out.mutation_marker = source.mutation_marker;
            out.is_constructor = source.is_constructor;
            out.constructor_typed = source.constructor_typed;
            out.type_parameters = source.type_parameters;
            out.external_symbol = source.external_symbol;
            out.foreign_export = source.foreign_export;
            out.type_constraints = source.type_constraints;

            for (const auto& parameter : source.parameters) {
                Parameter copy;
                copy.name = parameter.name;
                copy.type = materialize_type(parameter.type, substitution, deferred);
                copy.writable = parameter.writable;
                copy.span = parameter.span;
                copy.is_const = parameter.is_const;
                copy.is_local = parameter.is_local;
                copy.is_extension = parameter.is_extension;
                if (parameter.default_value) {
                    copy.default_value = clone_expr(
                        *parameter.default_value, substitution, deferred, current_class);
                }
                type_environment_[copy.name] = clone_type(copy.type);
                out.parameters.push_back(std::move(copy));
            }
            for (const auto& statement : source.body) {
                out.body.push_back(clone_stmt(*statement, substitution, deferred, current_class));
            }
            type_environment_ = saved_environment;traversal_expected_result_=saved_expected;
            return out;
        } catch (...) {
            type_environment_ = saved_environment;traversal_expected_result_=saved_expected;
            throw;
        }
    }

    void materialize_concrete_class(const ClassDecl& source) {
        if (class_index_.contains(source.name)) return;
        build_class(source, source.name, {});
    }

    std::string instantiate_class(const std::string& name, const std::vector<TypeName>& source_arguments) {
        const auto templ = class_templates_.find(name);
        if (templ == class_templates_.end()) {
            frontend_error("UNKNOWN_GENERIC", "Unknown generic class '" + name + "'.");
        }

        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) arguments.push_back(materialize_type(argument, {}, {}));

        if (arguments.size() != templ->second->type_parameters.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic class '" + name + "' expects " +
                               std::to_string(templ->second->type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           templ->second->span);
        }
        validate_generic_arguments(
            templ->second->type_parameters, templ->second->type_constraints,
            arguments, "class", name, templ->second->span);

        if (name == standard_class::map && !arguments.empty() &&
            !standard_collection_key_type(arguments[0])) {
            frontend_error("STANDARD_KEY_TYPE",
                           "map.Map keys must be integer, bool, string, or tuples of these values.",
                           source_arguments[0].span);
        }
        if (name == standard_class::set && !arguments.empty() &&
            !standard_collection_key_type(arguments[0])) {
            frontend_error("STANDARD_KEY_TYPE",
                           "set.Set values must be integer, bool, string, or tuples of these values.",
                           source_arguments[0].span);
        }

        const auto key = instance_key(name, arguments);
        if (const auto existing = class_instances_.find(key); existing != class_instances_.end()) {
            return existing->second;
        }

        const auto concrete_name = mangle(abi::generic_class_kind, name, key);
        class_instances_[key] = concrete_name;
        TypeName instance_type;
        instance_type.name = name;
        for (const auto& argument : arguments) {
            instance_type.arguments.push_back(clone_type(argument));
        }
        class_instance_types_.emplace(concrete_name, std::move(instance_type));

        Substitution substitution;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            substitution[templ->second->type_parameters[i]] = clone_type(arguments[i]);
        }

        build_class(*templ->second, concrete_name, substitution);
        return concrete_name;
    }

    void build_class(
        const ClassDecl& source,
        const std::string& concrete_name,
        const Substitution& class_substitution) {
        if (class_index_.contains(concrete_name)) return;
        ModuleScope module_scope(current_module_namespace_, source.module_namespace);

        ClassDecl out;
        out.name = concrete_name;
        out.source_file = source.source_file;
        out.module_namespace = source.module_namespace;
        out.span = source.span;
        out.standard_library = source.standard_library;
        out.self_name = source.self_name;
        out.self_span = source.self_span;
        out.is_private = source.is_private;

        for (const auto& field : source.fields) {
            FieldDecl copy;
            copy.name = field.name;
            copy.type = materialize_type(field.type, class_substitution, {});
            copy.span = field.span;
            copy.is_const = field.is_const;
            copy.is_private = field.is_private;
            copy.reference = field.reference;
            if (field.default_value) {
                copy.default_value = clone_expr(*field.default_value, class_substitution, {}, concrete_name);
            }
            out.fields.push_back(std::move(copy));
        }

        output_.classes.push_back(std::move(out));
        class_index_[concrete_name] = output_.classes.size() - 1;

        std::unordered_map<std::string, std::vector<const FunctionDecl*>>
            source_method_families;
        for (const auto& method : source.methods) {
            if (!method.is_constructor) {
                source_method_families[method.name].push_back(&method);
            }
        }

        for (const auto& [name, family] : source_method_families) {
            if (family.size() <= 1) continue;
            auto& target = method_specialization_families_[concrete_name][name];
            for (const auto* method : family) {
                target.push_back(make_method_specialization_member(
                    *method, class_substitution));
            }
        }

        for (const auto& method : source.methods) {
            const bool specialized_family =
                !method.is_constructor &&
                source_method_families.at(method.name).size() > 1;
            if (specialized_family) continue;
            if (method.type_parameters.empty()) {
                method_return_types_[concrete_name][method.name] =
                    materialize_type(method.return_type, class_substitution, {},true);
            } else {
                const auto deferred = set_of(method.type_parameters);
                auto method_template =
                    clone_function(method, class_substitution, deferred, concrete_name);
                generic_methods_[concrete_name][method.name] =
                    std::move(method_template);
            }
        }

        for (const auto& method : source.methods) {
            const bool specialized_family =
                !method.is_constructor &&
                source_method_families.at(method.name).size() > 1;
            if (specialized_family || !method.type_parameters.empty()) continue;
            auto copy = clone_function(method, class_substitution, {}, concrete_name);
            if ((source.name == standard_class::map || source.name == standard_class::set) &&
                method.name == "__hash") {
                const auto parameter = source.name == standard_class::map ? "K" : "T";
                if (const auto key = class_substitution.find(parameter);
                    key != class_substitution.end()) {
                    if (auto body = standard_collection_hash_body(
                            key->second, copy.parameters.front().name)) {
                        copy.body = std::move(*body);
                    }
                }
            }
            copy.type_parameters.clear();
            output_.classes[class_index_.at(concrete_name)].methods.push_back(std::move(copy));
        }
    }

    std::string instantiate_function(
        const FunctionDecl& function,
        const std::vector<TypeName>& source_arguments) {
        const auto& name = function.name;

        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments)
            arguments.push_back(materialize_type(argument, {}, {}));

        if (arguments.size() != function.type_parameters.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic function '" + name + "' expects " +
                               std::to_string(function.type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           function.span);
        }
        validate_generic_arguments(
            function.type_parameters, function.type_constraints,
            arguments, "function", name, function.span);

        const auto key = function_declaration_key(function) + instance_key(name, arguments);
        if (const auto existing = function_instances_.find(key);
            existing != function_instances_.end()) {
            return existing->second;
        }

        const auto concrete_name = mangle("gf", name, key);
        function_instances_[key] = concrete_name;

        Substitution substitution;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            substitution[function.type_parameters[i]] = clone_type(arguments[i]);
        }

        function_return_types_[concrete_name] =
            materialize_type(function.return_type, substitution, {},true);
        auto concrete = clone_function(function, substitution, {}, "");
        concrete.name = concrete_name;
        concrete.type_parameters.clear();
        concrete.type_constraints.clear();
        output_.functions.push_back(std::move(concrete));
        return concrete_name;
    }

    std::string instantiate_function(
        const std::string& name,
        const std::vector<TypeName>& source_arguments) {
        const auto templ = function_templates_.find(name);
        if (templ == function_templates_.end()) {
            frontend_error("UNKNOWN_GENERIC", "Unknown generic function '" + name + "'.");
        }
        return instantiate_function(*templ->second, source_arguments);
    }


    std::string materialize_concrete_method_specialization(
        const std::string& class_name,
        MethodSpecializationMember& member) {
        const auto key =
            class_name + "::" + function_declaration_key(member.signature);
        auto& instances = method_specialization_instances_[class_name];
        if (const auto existing = instances.find(key);
            existing != instances.end()) {
            return existing->second;
        }

        const auto concrete_name =
            mangle("ms", member.signature.name, key);
        instances[key] = concrete_name;
        method_return_types_[class_name][concrete_name] =
            materialize_type(
                member.source->return_type,
                member.class_substitution,
                {});
        auto concrete = clone_function(
            *member.source, member.class_substitution, {}, class_name);
        concrete.name = concrete_name;
        concrete.type_parameters.clear();
        concrete.type_constraints.clear();
        output_.classes[class_index_.at(class_name)].methods.push_back(
            std::move(concrete));
        return concrete_name;
    }

    std::string instantiate_method_specialization(
        const std::string& class_name,
        MethodSpecializationMember& member,
        const std::vector<TypeName>& source_arguments) {
        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) {
            arguments.push_back(materialize_type(argument, {}, {}));
        }
        if (arguments.size() != member.signature.type_parameters.size()) {
            frontend_error(
                "GENERIC_ARITY",
                "Generic method '" + member.signature.name + "' expects " +
                    std::to_string(member.signature.type_parameters.size()) +
                    " type argument(s), got " +
                    std::to_string(arguments.size()) + ".",
                member.signature.span);
        }
        validate_generic_arguments(
            member.signature.type_parameters,
            member.signature.type_constraints,
            arguments,
            "method",
            member.signature.name,
            member.signature.span);

        const auto key =
            class_name + "::" + function_declaration_key(member.signature) +
            instance_key(member.signature.name, arguments);
        auto& instances = method_specialization_instances_[class_name];
        if (const auto existing = instances.find(key);
            existing != instances.end()) {
            return existing->second;
        }

        Substitution combined = clone_substitution(member.class_substitution);
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            combined[member.signature.type_parameters[i]] =
                clone_type(arguments[i]);
        }

        const auto concrete_name =
            mangle("ms", member.signature.name, key);
        instances[key] = concrete_name;
        method_return_types_[class_name][concrete_name] =
            materialize_type(member.source->return_type, combined, {},true);
        auto concrete =
            clone_function(*member.source, combined, {}, class_name);
        concrete.name = concrete_name;
        concrete.type_parameters.clear();
        concrete.type_constraints.clear();
        output_.classes[class_index_.at(class_name)].methods.push_back(
            std::move(concrete));
        return concrete_name;
    }

    std::string resolve_method_family(
        const std::string& class_name,
        const std::string& name,
        const std::vector<CallArg>& args,
        const std::string& current_class,
        SourceSpan span) {
        auto& family =
            method_specialization_families_.at(class_name).at(name);

        std::vector<MethodSpecializationMember*> concrete_matches;
        struct GenericMethodMatch {
            MethodSpecializationMember* member{};
            std::vector<TypeName> arguments;
        };
        std::vector<GenericMethodMatch> generic_matches;

        for (auto& member : family) {
            if (member.signature.type_parameters.empty()) {
                if (concrete_family_match(
                        member.signature, args, current_class)) {
                    concrete_matches.push_back(&member);
                }
            } else if (const auto inferred =
                           generic_family_match(
                               member.signature, args, current_class)) {
                generic_matches.push_back(
                    GenericMethodMatch{&member, *inferred});
            }
        }

        if (concrete_matches.size() > 1) {
            frontend_error(
                "AMBIGUOUS_SPECIALIZATION",
                "Call to method '" + name +
                    "' matches more than one concrete specialization.",
                span);
        }
        if (concrete_matches.size() == 1) {
            return materialize_concrete_method_specialization(
                class_name, *concrete_matches.front());
        }
        if (generic_matches.empty()) {
            frontend_error(
                "SPECIALIZATION_NO_MATCH",
                "No specialization of method '" + name +
                    "' matches the statically known argument types.",
                span);
        }

        std::vector<const FunctionDecl*> candidates;
        for (const auto& match : generic_matches) {
            candidates.push_back(&match.member->signature);
        }
        const auto* chosen = most_specific_generic(candidates, span, name);
        for (auto& match : generic_matches) {
            if (&match.member->signature == chosen) {
                return instantiate_method_specialization(
                    class_name, *match.member, match.arguments);
            }
        }
        throw std::logic_error(
            "Generic method specialization selection lost its candidate.");
    }

    std::string resolve_method_family_explicit(
        const std::string& class_name,
        const std::string& name,
        const std::vector<TypeName>& source_arguments,
        const std::vector<CallArg>& args,
        const std::string& current_class,
        SourceSpan span) {
        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) {
            arguments.push_back(materialize_type(argument, {}, {}));
        }

        auto& family =
            method_specialization_families_.at(class_name).at(name);
        std::vector<const FunctionDecl*> candidates;
        for (auto& member : family) {
            if (member.signature.type_parameters.empty()) continue;
            if (member.signature.type_parameters.size() != arguments.size()) continue;
            if (generic_arguments_satisfy(member.signature, arguments)) {
                candidates.push_back(&member.signature);
            }
        }
        if (candidates.empty()) {
            frontend_error(
                "GENERIC_CONSTRAINT",
                "Explicit type arguments do not match any generic specialization of method '" +
                    name + "'.",
                span);
        }

        filter_explicit_container_domains(candidates, args, current_class);
        if (candidates.empty()) {
            frontend_error("SPECIALIZATION_NO_MATCH",
                "No specialization of method '" + name + "' matches the statically known argument types.",
                span);
        }
        const auto* chosen = most_specific_generic(candidates, span, name);
        for (auto& member : family) {
            if (&member.signature == chosen) {
                return instantiate_method_specialization(
                    class_name, member, arguments);
            }
        }
        throw std::logic_error(
            "Explicit generic method specialization selection lost its candidate.");
    }

    std::string method_name(const std::string& name, const std::vector<TypeName>& arguments) const {
        return mangle("gm", name, instance_key(name, arguments));
    }

    void request_method_for_class(
        const std::string& receiver_class,
        const std::string& name,
        const std::vector<TypeName>& arguments,
        SourceSpan span) {
        const auto owner = generic_method_owner(receiver_class, name);
        if (!owner) {
            frontend_error("UNKNOWN_GENERIC_METHOD",
                           "Class '" + receiver_class + "' has no generic method named '" + name + "'.",
                           span);
        }

        const auto& templ = generic_methods_.at(*owner).at(name);
        if (templ.type_parameters.size() != arguments.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic method '" + name + "' expects " +
                               std::to_string(templ.type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           span);
        }
        instantiate_method_for_class(*owner, name, arguments);
    }

    void instantiate_method_for_class(
        const std::string& class_name,
        const std::string& method,
        const std::vector<TypeName>& source_arguments) {
        const auto class_methods = generic_methods_.find(class_name);
        if (class_methods == generic_methods_.end()) return;
        const auto method_it = class_methods->second.find(method);
        if (method_it == class_methods->second.end()) return;

        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) arguments.push_back(materialize_type(argument, {}, {}));

        const auto& templ = method_it->second;
        if (arguments.size() != templ.type_parameters.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic method '" + method + "' expects " +
                               std::to_string(templ.type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           templ.span);
        }
        validate_generic_arguments(
            templ.type_parameters, templ.type_constraints, arguments,
            "method", method, templ.span);

        const auto key = instance_key(method, arguments);
        auto& done = instantiated_methods_[class_name];
        if (!done.insert(key).second) return;

        Substitution substitution;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            substitution[templ.type_parameters[i]] = clone_type(arguments[i]);
        }

        const auto concrete_method_name = method_name(method, arguments);
        method_return_types_[class_name][concrete_method_name] =
            materialize_type(templ.return_type, substitution, {},true);
        auto concrete = clone_function(templ, substitution, {}, class_name);
        concrete.name = concrete_method_name;
        concrete.type_parameters.clear();
        concrete.type_constraints.clear();
        output_.classes[class_index_.at(class_name)].methods.push_back(std::move(concrete));
    }
};

} // namespace

ResolvedProgram load_program_with_modules(
    const std::filesystem::path& root_file,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors,
    CompileInputs* inputs) {
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    std::map<std::string, fs::path> packages;
    auto program = ModuleLoader(
        command_working_directory, max_errors, std::nullopt, &packages, true,
        &compiler_extensions, inputs)
        .load(root_file);
    return ResolvedProgram{
        std::move(program), std::move(compiler_extensions), std::move(packages)};
}

ResolvedProgram load_program_with_root_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors,
    bool enforce_package_lock) {
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    std::map<std::string, fs::path> packages;
    auto program = ModuleLoader(
        command_working_directory, max_errors, std::string(root_source),
        &packages, enforce_package_lock, &compiler_extensions)
        .load(root_file);
    return ResolvedProgram{
        std::move(program), std::move(compiler_extensions), std::move(packages)};
}

std::map<std::string, fs::path> resolve_package_dependencies(
    const std::filesystem::path& root_file,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors) {
    std::map<std::string, fs::path> packages;
    (void)ModuleLoader(
        command_working_directory, max_errors, std::nullopt, &packages, false)
        .load(root_file);
    return packages;
}

std::map<std::string, fs::path> resolve_package_dependencies_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors) {
    std::map<std::string, fs::path> packages;
    (void)ModuleLoader(
        command_working_directory, max_errors, std::string(root_source),
        &packages, false)
        .load(root_file);
    return packages;
}

void validate_same_shape_declarations(const ResolvedProgram& program) {
    for (const auto& extension : program.compiler_extensions) {
        if (extension.schema_revision == 2) continue;
        for (const auto& [table, fields] : extension.tables) {
            if (!table.starts_with("specialization.") && !table.starts_with("backend.") &&
                !table.starts_with("memory.")) continue;
            const auto relation = fields.find("same_shape");
            if (relation == fields.end()) continue;
            std::vector<std::string> parameters;
            try { parameters = shape_relation_parameters(relation->second); }
            catch (const std::exception& error) {
                frontend_error("PACKAGE_COMPILER_EXTENSION", error.what(), {});
            }
            const auto operation = fields.find("operation");
            if (operation == fields.end())
                frontend_error("PACKAGE_COMPILER_EXTENSION", "same_shape requires an operation", {});
            const auto entry = extension.tables.find("operation." + operation->second);
            if (entry == extension.tables.end() || !entry->second.contains("function"))
                frontend_error("PACKAGE_COMPILER_EXTENSION", "same_shape requires a declared operation function", {});
            auto configured = entry->second.at("function");
            for (const auto member_prefix :
                 {member_function_name::method_prefix, member_function_name::constructor_prefix}) {
                if (configured.starts_with(member_prefix)) {
                    configured.erase(0, member_prefix.size());
                    break;
                }
            }
            const auto prefix = extension.package + ".";
            if (configured.starts_with(prefix)) configured.erase(0, prefix.size());
            bool matched = false;
            const auto validate = [&](const FunctionDecl& function, const std::string& name) {
                const auto relative = fs::path(function.source_file).lexically_normal().lexically_relative(
                    fs::path(extension.package_root).lexically_normal());
                if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
                    (name != configured && !name.ends_with("." + configured))) return;
                matched = true;
                for (const auto& parameter_name : parameters) {
                    const auto parameter = std::find_if(function.parameters.begin(), function.parameters.end(),
                        [&](const auto& candidate) { return candidate.name == parameter_name; });
                    if (parameter == function.parameters.end() || parameter->type.name != "tensor" ||
                        parameter->type.array_depth != 0)
                        frontend_error("PACKAGE_COMPILER_EXTENSION", "same_shape operand '" + parameter_name +
                            "' must name a tensor parameter of operation '" + operation->second + "'", function.span);
                }
            };
            for (const auto& function : program.program.functions) validate(function, function.name);
            for (const auto& declaration : program.program.classes) {
                std::size_t constructor = 0;
                for (const auto& method : declaration.methods) {
                    validate(method, declaration.name + "." +
                        (method.is_constructor ? std::to_string(constructor++) : method.name));
                }
            }
            if (!matched)
                frontend_error("PACKAGE_COMPILER_EXTENSION", "same_shape operation function is not declared", {});
        }
    }
}

ConcreteProgram expand_generics(ResolvedProgram program) {
    optimizer::validate_extension_declarations(program.compiler_extensions, program.program.declaration_catalog);
    validate_same_shape_declarations(program);
    return ConcreteProgram{
        GenericExpander(std::move(program.program)).run(),
        std::move(program.compiler_extensions)};
}

} // namespace quidra
