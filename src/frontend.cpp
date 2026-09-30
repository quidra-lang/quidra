#include "quidra/frontend.hpp"

#include "quidra/diagnostic.hpp"
#include "quidra/import_path.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/project.hpp"
#include "quidra/toml_subset.hpp"

#include "nesting_budget.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
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

namespace fs = std::filesystem;

[[noreturn]] void frontend_error(std::string code, std::string message, SourceSpan span = {}) {
    throw CompileError(Diagnostic{std::move(code), std::move(message), span});
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read Quidra source: " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
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
};

ExprPtr clone_package_constant_expression(const Expr& source, SourceSpan use_span) {
    auto result = std::make_unique<Expr>();
    result->span = use_span;
    result->contextual_default_type = source.contextual_default_type;
    if (const auto* value = std::get_if<IntegerExpr>(&source.data)) {
        result->data = *value;
        return result;
    }
    if (const auto* value = std::get_if<FloatExpr>(&source.data)) {
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
            clone_package_constant_expression(*binary->right, use_span)};
        return result;
    }
    if (const auto* call = std::get_if<CallExpr>(&source.data)) {
        static constexpr std::array<std::string_view, 13> numeric_casts{{
            "int8", "int16", "int32", "int", "int64",
            "uint8", "uint16", "uint32", "uint64", "bigint",
            "float32", "float", "bigreal"}};
        const bool scalar_numeric_cast =
            std::find(numeric_casts.begin(), numeric_casts.end(), call->callee) !=
            numeric_casts.end();
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
        if (receiver && receiver->name == "exact" && call->method == "atom" &&
            call->type_arguments.empty() && call->args.size() == 2 &&
            !call->args[0].writable && !call->args[1].writable) {
            auto cloned_receiver = std::make_unique<Expr>();
            cloned_receiver->span = use_span;
            cloned_receiver->data = NameExpr{"exact"};
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
        exports.classes.emplace("Handle", "$std.file.Handle");
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
        exports.classes.emplace("Instant", "$std.time.Instant");
        exports.classes.emplace("Duration", "$std.time.Duration");
        for (const char* name : {"now", "since", "seconds", "sleep"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "gpu") {
        exports.functions.emplace("sync", std::string(*standard_function_target(module, "sync")));
    } else if (module == "task") {
        exports.functions.emplace("all", std::string(*standard_function_target(module, "all")));
    } else if (module == "atomic") {
        exports.classes.emplace("Counter", "$std.atomic.Counter");
        exports.functions.emplace("counter", std::string(*standard_function_target(module, "counter")));
    } else if (module == "autograd") {
        exports.classes.emplace("Target", "$std.autograd.Target");
        exports.functions.emplace("target", std::string(*standard_function_target(module, "target")));
    } else if (module == "ref") {
        exports.classes.emplace("Cell", "$std.ref.Cell");
    } else if (module == "reflect") {
        for (const char* name : {"collect", "paths", "type_name"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "random") {
        exports.classes.emplace("Generator", "$std.random.Generator");
        exports.functions.emplace("generator", std::string(*standard_function_target(module, "generator")));
    } else if (module == "process") {
        exports.classes.emplace("Result", "$std.process.Result");
        for (const char* name : {"run", "shell", "exit"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "map") {
        exports.classes.emplace("Map", "$std.map.Map");
    } else if (module == "set") {
        exports.classes.emplace("Set", "$std.set.Set");
    } else if (module == "json") {
        exports.classes.emplace("Value", "$std.json.Value");
        exports.functions.emplace("parse", std::string(*standard_function_target(module, "parse")));
    } else if (module == "http") {
        exports.classes.emplace("Response", "$std.http.Response");
        exports.functions.emplace("get", std::string(*standard_function_target(module, "get")));
    } else if (module == "tensor") {
        exports.functions.emplace("zeros", std::string(*standard_function_target(module, "zeros")));
        exports.functions.emplace("ones", std::string(*standard_function_target(module, "ones")));
    } else if (module == "exact") {
        exports.functions.emplace("atom", std::string(*standard_function_target(module, "atom")));
        exports.functions.emplace("unary", std::string(*standard_function_target(module, "unary")));
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

ExprPtr standard_member(ExprPtr base, std::string name) {
    auto expression = std::make_unique<Expr>();
    expression->span = standard_span();
    expression->data = MemberExpr{std::move(base), std::move(name)};
    return expression;
}

ExprPtr standard_unary(std::string op, ExprPtr operand) {
    auto expression = std::make_unique<Expr>();
    expression->span = standard_span();
    expression->data = UnaryExpr{std::move(op), std::move(operand)};
    return expression;
}

ExprPtr standard_binary(std::string op, ExprPtr left, ExprPtr right) {
    auto expression = std::make_unique<Expr>();
    expression->span = standard_span();
    expression->data = BinaryExpr{std::move(op), std::move(left), std::move(right)};
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
    if (module == "atomic") {
        ClassDecl counter;
        counter.name = "$std.atomic.Counter";
        counter.span = standard_span();
        counter.fields.push_back(standard_field("$handle", "uint64"));
        declarations.push_back(std::move(counter));
    } else if (module == "autograd") {
        ClassDecl target;
        target.name = "$std.autograd.Target";
        target.span = standard_span();
        target.fields.push_back(standard_field("$handle", "uint64"));
        declarations.push_back(std::move(target));
    } else if (module == "ref") {
        TypeName generic_t = standard_type("T");
        TypeName cell_t = standard_type("$std.ref.Cell");
        cell_t.arguments.push_back(generic_t);

        ClassDecl cell;
        cell.name = "$std.ref.Cell";
        cell.span = standard_span();
        cell.type_parameters.push_back("T");

        FieldDecl value;
        value.name = "value";
        value.type = generic_t;
        value.span = standard_span();
        cell.fields.push_back(std::move(value));

        Parameter other;
        other.name = "other";
        other.type = cell_t;
        other.span = standard_span();

        auto receiver_address = standard_unary("&", standard_name("value"));
        auto other_address = standard_unary(
            "&", standard_member(standard_name("other"), "value"));
        std::vector<Parameter> same_parameters;
        same_parameters.push_back(std::move(other));
        cell.methods.push_back(standard_method(
            "same", std::move(same_parameters), "bool",
            standard_binary("==", std::move(receiver_address), std::move(other_address))));
        declarations.push_back(std::move(cell));
    } else if (module == "file") {
        ClassDecl handle;
        handle.name = "$std.file.Handle";
        handle.span = standard_span();
        handle.fields.push_back(standard_field("$handle", "uint64"));
        declarations.push_back(std::move(handle));
    } else if (module == "time") {
        ClassDecl instant;
        instant.name = "$std.time.Instant";
        instant.span = standard_span();
        instant.fields.push_back(standard_field("$seconds", "float"));
        declarations.push_back(std::move(instant));

        ClassDecl duration;
        duration.name = "$std.time.Duration";
        duration.span = standard_span();
        duration.fields.push_back(standard_field("$seconds", "float"));
        duration.methods.push_back(standard_method(
            "seconds", std::vector<Parameter>{}, "float", standard_name("$seconds")));
        declarations.push_back(std::move(duration));
    } else if (module == "random") {
        ClassDecl generator;
        generator.name = "$std.random.Generator";
        generator.span = standard_span();
        generator.fields.push_back(standard_field("$state", "uint64"));

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
            "float", std::vector<Parameter>{}, "float", standard_call("$std.random.float")));
        generator.methods.push_back(standard_method(
            "bool", std::vector<Parameter>{}, "bool", standard_call("$std.random.bool")));
        declarations.push_back(std::move(generator));
    } else if (module == "process") {
        ClassDecl result;
        result.name = "$std.process.Result";
        result.span = standard_span();
        result.fields.push_back(standard_field("status", "int"));
        result.fields.push_back(standard_field("output", "string"));
        result.fields.push_back(standard_field("error", "string"));
        result.fields.push_back(standard_field("started", "bool"));
        declarations.push_back(std::move(result));
    } else if (module == "map") {
        constexpr std::string_view source = R"QUI(class Map<K, V>
    K[] __keys = []
    V[] __values = []
    int[] __hashes = []
    bool[] __active = []
    int[] __slots = array(8, fill = -1)
    int __size = 0
    int __tombstones = 0
    int __empty_slot = -1
    int __last_index = -1
    int __last_hash = -1
    int __last_slot = -1
    K[] __last_key = []
    int __version = 0
    int __last_version = -1

    int __hash(K key)
        int[] __encoded = key.string().codepoints()
        int __result = 0
        for __unit in __encoded
            __result = (__result * 131 + __unit) % 2147483647
        return __result

    int __slot_of(K key, int hash)
        int __capacity = len(__slots)
        int __slot = hash AND (__capacity - 1)
        int __scanned = 0
        int __first_tombstone = -1
        while __scanned < __capacity
            int __index = __slots[__slot]
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
            __slot = (__slot + 1) AND (__capacity - 1)
            __scanned += 1
        __empty_slot = __first_tombstone
        return -1

    int __find(K key, int hash)
        int __slot = __slot_of(key, hash)
        if __slot < 0
            return -1
        return __slots[__slot]

    void __place(int index)
        int __capacity = len(__slots)
        int __slot = __hashes[index] AND (__capacity - 1)
        while __slots[__slot] >= 0
            __slot = (__slot + 1) AND (__capacity - 1)
        __slots[__slot] = index

    void __rehash(int capacity)
        __slots = array(capacity, fill = -1)
        for __index in range(len(__keys))
            if __active[__index]
                __place(__index)
        __tombstones = 0
        return void

    void __compact()
        K[] __new_keys = array(__size)
        V[] __new_values = array(__size)
        int[] __new_hashes = array(__size, fill = 0)
        bool[] __new_active = array(__size, fill = false)
        int __write = 0
        for __index in range(len(__keys))
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
        __rehash(len(__slots))
        __version += 1
        __last_version = -1
        return void

    bool has(K key)
        int __hash_value = __hash(key)
        return __find(key, __hash_value) >= 0

    V | none get(K key)
        int __hash_value = __hash(key)
        int __index = __find(key, __hash_value)
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

    void set(K key, V value)
        int __hash_value = -1
        if __last_version == __version and __last_index >= 0
            int __cached = __last_index
            if __keys[__cached] == key
                __values[__cached] = value
                return void

        bool __reuse_missing = false
        if __last_version == __version and __last_index < 0 and len(__last_key) == 1
            if __last_key[0] == key
                __reuse_missing = true
                __hash_value = __last_hash

        int __index = -1
        int __insert_slot = -1
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

        int __new_index = len(__keys)
        __keys = __keys.append(key)
        __values = __values.append(value)
        __hashes = __hashes.append(__hash_value)
        __active = __active.append(true)
        __size += 1

        if __size > len(__slots) - len(__slots) / 4
            __rehash(len(__slots) * 2)
        else
            if __slots[__insert_slot] == -2
                __tombstones -= 1
            __slots[__insert_slot] = __new_index
        __version += 1
        __last_version = -1
        return void

    bool remove(K key)
        int __hash_value = __hash(key)
        int __slot = __slot_of(key, __hash_value)
        if __slot < 0
            return false

        int __removed_index = __slots[__slot]
        __active[__removed_index] = false
        __slots[__slot] = -2
        __size -= 1
        __tombstones += 1
        __version += 1
        __last_version = -1
        if len(__keys) > 64 and __size * 2 <= len(__keys)
            __compact()
        elif __tombstones > len(__slots) / 4
            __rehash(len(__slots))
        return true

    int size()
        return __size

    K[] keys()
        if __size != len(__keys)
            __compact()
        return __keys

    V[] values()
        if __size != len(__values)
            __compact()
        return __values
)QUI";
        Parser parser(Lexer(std::string(source)).scan(), 20);
        auto program = parser.parse();
        if (program.classes.size() != 1) {
            throw std::logic_error("invalid built-in map declaration");
        }
        auto map = std::move(program.classes.front());
        map.name = "$std.map.Map";
        map.span = standard_span();
        declarations.push_back(std::move(map));
    } else if (module == "http") {
        ClassDecl response;
        response.name = "$std.http.Response";
        response.span = standard_span();
        response.fields.push_back(standard_field("status", "int"));
        response.fields.push_back(standard_field("body", "bin"));
        response.fields.push_back(standard_field("$headers", "uint64"));

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
        value.name = "$std.json.Value";
        value.span = standard_span();
        value.fields.push_back(standard_field("$handle", "uint64"));

        value.methods.push_back(standard_method(
            "kind", std::vector<Parameter>{}, "string", standard_call("$std.json.kind")));
        value.methods.push_back(standard_method(
            "size", std::vector<Parameter>{},
            standard_union_type({"int", "error"}), standard_call("$std.json.size")));

        std::vector<Parameter> get_parameters;
        get_parameters.push_back(standard_parameter("key", "string"));
        std::vector<CallArg> get_arguments;
        get_arguments.push_back(standard_arg(standard_name("key")));
        value.methods.push_back(standard_method(
            "get", std::move(get_parameters),
            standard_union_type({"$std.json.Value", "none", "error"}),
            standard_call("$std.json.get", std::move(get_arguments))));

        std::vector<Parameter> at_parameters;
        at_parameters.push_back(standard_parameter("index", "int"));
        std::vector<CallArg> at_arguments;
        at_arguments.push_back(standard_arg(standard_name("index")));
        value.methods.push_back(standard_method(
            "at", std::move(at_parameters),
            standard_union_type({"$std.json.Value", "none", "error"}),
            standard_call("$std.json.at", std::move(at_arguments))));

        value.methods.push_back(standard_method(
            "text", std::vector<Parameter>{},
            standard_union_type({"string", "error"}), standard_call("$std.json.text")));
        value.methods.push_back(standard_method(
            "integer", std::vector<Parameter>{},
            standard_union_type({"int", "error"}), standard_call("$std.json.integer")));
        value.methods.push_back(standard_method(
            "number", std::vector<Parameter>{},
            standard_union_type({"float", "error"}), standard_call("$std.json.number")));
        value.methods.push_back(standard_method(
            "bigint", std::vector<Parameter>{},
            standard_union_type({"bigint", "error"}), standard_call("$std.json.bigint")));
        value.methods.push_back(standard_method(
            "bigreal", std::vector<Parameter>{},
            standard_union_type({"bigreal", "error"}), standard_call("$std.json.bigreal")));
        value.methods.push_back(standard_method(
            "boolean", std::vector<Parameter>{},
            standard_union_type({"bool", "error"}), standard_call("$std.json.boolean")));
        value.methods.push_back(standard_method(
            "encode", std::vector<Parameter>{}, "string", standard_call("$std.json.encode")));

        std::vector<Parameter> equal_parameters;
        equal_parameters.push_back(standard_parameter("other", "$std.json.Value"));
        std::vector<CallArg> equal_arguments;
        equal_arguments.push_back(standard_arg(standard_name("other")));
        value.methods.push_back(standard_method(
            "equal", std::move(equal_parameters), "bool",
            standard_call("$std.json.equal", std::move(equal_arguments))));
        declarations.push_back(std::move(value));
    } else if (module == "set") {
        constexpr std::string_view source = R"QUI(class Set<T>
    T[] __values = []
    int[] __hashes = []
    bool[] __active = []
    int[] __slots = array(8, fill = -1)
    int __size = 0
    int __tombstones = 0
    int __empty_slot = -1

    int __hash(T value)
        int[] __encoded = value.string().codepoints()
        int __result = 0
        for __unit in __encoded
            __result = (__result * 131 + __unit) % 2147483647
        return __result

    int __slot_of(T value, int hash)
        int __capacity = len(__slots)
        int __slot = hash AND (__capacity - 1)
        int __scanned = 0
        int __first_tombstone = -1
        while __scanned < __capacity
            int __index = __slots[__slot]
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
            __slot = (__slot + 1) AND (__capacity - 1)
            __scanned += 1
        __empty_slot = __first_tombstone
        return -1

    int __find(T value, int hash)
        int __slot = __slot_of(value, hash)
        if __slot < 0
            return -1
        return __slots[__slot]

    void __place(int index)
        int __capacity = len(__slots)
        int __slot = __hashes[index] AND (__capacity - 1)
        while __slots[__slot] >= 0
            __slot = (__slot + 1) AND (__capacity - 1)
        __slots[__slot] = index

    void __rehash(int capacity)
        __slots = array(capacity, fill = -1)
        for __index in range(len(__values))
            if __active[__index]
                __place(__index)
        __tombstones = 0
        return void

    void __compact()
        T[] __new_values = array(__size)
        int[] __new_hashes = array(__size, fill = 0)
        bool[] __new_active = array(__size, fill = false)
        int __write = 0
        for __index in range(len(__values))
            if __active[__index]
                __new_values[__write] = __values[__index]
                __new_hashes[__write] = __hashes[__index]
                __new_active[__write] = true
                __write += 1
        __values = __new_values
        __hashes = __new_hashes
        __active = __new_active
        __rehash(len(__slots))
        return void

    bool has(T value)
        int __hash_value = __hash(value)
        return __find(value, __hash_value) >= 0

    void add(T value)
        int __hash_value = __hash(value)
        if __find(value, __hash_value) >= 0
            return void

        int __new_index = len(__values)
        int __insert_slot = __empty_slot
        __values = __values.append(value)
        __hashes = __hashes.append(__hash_value)
        __active = __active.append(true)
        __size += 1

        if __size > len(__slots) - len(__slots) / 4
            __rehash(len(__slots) * 2)
        else
            if __slots[__insert_slot] == -2
                __tombstones -= 1
            __slots[__insert_slot] = __new_index
        return void

    bool remove(T value)
        int __hash_value = __hash(value)
        int __slot = __slot_of(value, __hash_value)
        if __slot < 0
            return false

        int __removed_index = __slots[__slot]
        __active[__removed_index] = false
        __slots[__slot] = -2
        __size -= 1
        __tombstones += 1
        if len(__values) > 64 and __size * 2 <= len(__values)
            __compact()
        elif __tombstones > len(__slots) / 4
            __rehash(len(__slots))
        return true

    int size()
        return __size

    T[] values()
        if __size != len(__values)
            __compact()
        return __values
)QUI";
        Parser parser(Lexer(std::string(source)).scan(), 20);
        auto program = parser.parse();
        if (program.classes.size() != 1) {
            throw std::logic_error("invalid built-in set declaration");
        }
        auto set = std::move(program.classes.front());
        set.name = "$std.set.Set";
        set.span = standard_span();
        declarations.push_back(std::move(set));
    }
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
    if (type.name == "union") {
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
    if (auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
        for (auto& item : node->expressions) {
            rename_expr(*item, ns, local_classes, local_functions, imports, type_parameters);
        }
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
                expression.data = std::move(replacement->data);
                expression.contextual_default_type =
                    std::move(replacement->contextual_default_type);
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
        }
        if (auto* base = std::get_if<NameExpr>(&node->base->data)) {
            if (local_classes.contains(base->name)) base->name = qualify(ns, base->name);
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
    if (auto* node = std::get_if<TryExpr>(&expression.data)) {
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
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
                expression.data = std::move(call);
                return;
            }
        }

        if (receiver_name && local_classes.contains(receiver_name->name))
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
        if (aliases.contains(node->name)) {
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
    if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
        if (aliases.contains(node->name)) {
            frontend_error("SHADOWING", "Iteration binding shadows an import alias '" + node->name + "'.", statement.span);
        }
        for (const auto& child : node->body) reject_import_alias_shadow_stmt(*child, aliases);
        return;
    }
    if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
        for (const auto& match_case : node->cases) {
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
    if (auto* node = std::get_if<BindingStmt>(&statement.data)) {
        rename_type(node->declared_type, ns, local_classes, imports, type_parameters);
        if (node->value) rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<AssignStmt>(&statement.data)) {
        rename_expr(*node->target, ns, local_classes, local_functions, imports, type_parameters);
        rename_expr(*node->value, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<RebindStmt>(&statement.data)) {
        rename_expr(*node->target, ns, local_classes, local_functions, imports, type_parameters);
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
        for (auto& child : node->then_body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        for (auto& child : node->else_body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<MainGuardStmt>(&statement.data)) {
        for (auto& child : node->body)
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        return;
    }
    if (auto* node = std::get_if<WhileStmt>(&statement.data)) {
        rename_expr(*node->condition, ns, local_classes, local_functions, imports, type_parameters);
        for (auto& child : node->body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    if (auto* node = std::get_if<ForStmt>(&statement.data)) {
        rename_expr(*node->iterable, ns, local_classes, local_functions, imports, type_parameters);
        for (auto& child : node->body) {
            rename_stmt(*child, ns, local_classes, local_functions, imports, type_parameters);
        }
        return;
    }
    auto& node = std::get<MatchStmt>(statement.data);
    rename_expr(*node.value, ns, local_classes, local_functions, imports, type_parameters);
    for (auto& match_case : node.cases) {
        rename_type(match_case.type, ns, local_classes, imports, type_parameters);
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


std::string compact_type_spelling(const TypeName& type, std::string_view source) {
    const auto start = std::min(type.span.start.offset, source.size());
    const auto end = std::min(type.span.end.offset, source.size());
    std::string out;
    if (end > start) {
        for (const char c : source.substr(start, end - start)) {
            if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
        }
    }
    return out.empty() ? type.name : out;
}

bool same_parameter_contract(const Parameter& a, const Parameter& b, std::string_view source) {
    return a.name == b.name && a.writable == b.writable && a.is_const == b.is_const &&
           compact_type_spelling(a.type, source) == compact_type_spelling(b.type, source);
}

std::string function_prototype_key(const FunctionDecl& function, std::string_view source) {
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
    key += compact_type_spelling(function.return_type, source);
    key += "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) key += ",";
        const auto& parameter = function.parameters[i];
        if (parameter.is_const) key += "const ";
        key += compact_type_spelling(parameter.type, source);
        if (parameter.writable) key += "&";
        key += " ";
        key += parameter.name;
    }
    key += ")";
    return key;
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
        if (prototype.type_parameters != definition.type_parameters ||
            prototype.type_constraints != definition.type_constraints ||
            compact_type_spelling(prototype.return_type, source) != compact_type_spelling(definition.return_type, source) ||
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
            class_exposure_[declaration.name] =
                declaration.prototype_span ? declaration.prototype_span->start.offset
                                           : declaration.span.start.offset;
            class_completion_[declaration.name] = declaration.span.start.offset;
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
        if (const auto* node = std::get_if<TryExpr>(&expression.data)) {
            check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<StringTemplateExpr>(&expression.data)) {
            for (const auto& item : node->expressions) check_expr(*item, current_function);
        }
    }

    void check_stmt(const Stmt& statement, std::string_view current_function) const {
        if (const auto* node = std::get_if<BindingStmt>(&statement.data)) {
            if (node->declared_type.name != "auto")
                check_type(node->declared_type, statement.span.start.offset, true);
            if (node->value) check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<AssignStmt>(&statement.data)) {
            check_expr(*node->target, current_function);
            check_expr(*node->value, current_function);
            return;
        }
        if (const auto* node = std::get_if<RebindStmt>(&statement.data)) {
            check_expr(*node->target, current_function);
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
        if (const auto* node = std::get_if<WhileStmt>(&statement.data)) {
            check_expr(*node->condition, current_function);
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<ForStmt>(&statement.data)) {
            check_expr(*node->iterable, current_function);
            for (const auto& child : node->body) check_stmt(*child, current_function);
            return;
        }
        if (const auto* node = std::get_if<MatchStmt>(&statement.data)) {
            check_expr(*node->value, current_function);
            for (const auto& match_case : node->cases) {
                check_type(match_case.type, match_case.span.start.offset, false);
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
        std::vector<CompilerExtensionRegistration>* compiler_extensions = nullptr)
        : cwd_(fs::absolute(std::move(cwd)).lexically_normal()),
          max_errors_(max_errors ? max_errors : 1),
          root_source_(std::move(root_source)),
          resolved_packages_(resolved_packages),
          enforce_package_lock_(enforce_package_lock),
          compiler_extensions_(compiler_extensions) {}

    Program load(const fs::path& root) {
        Program merged;
        const auto absolute_root = root.is_absolute()
            ? root.lexically_normal()
            : (cwd_ / root).lexically_normal();
        merged.root_source_file = absolute_root.string();
        if (enforce_package_lock_) {
            try {
                package_lock_ = read_package_lock(cwd_);
            } catch (const std::exception& error) {
                frontend_error("PACKAGE_LOCK", error.what());
            }
            package_lock_loaded_ = true;
        }
        load_file(root, "", true, merged);
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
        return merged;
    }

private:
    fs::path cwd_;
    std::size_t max_errors_{20};
    std::optional<std::string> root_source_;
    std::map<std::string, fs::path>* resolved_packages_{};
    bool enforce_package_lock_{true};
    std::vector<CompilerExtensionRegistration>* compiler_extensions_{};
    bool package_lock_loaded_{};
    std::optional<PackageLockEntries> package_lock_;
    std::unordered_map<std::string, std::string> package_hash_cache_;
    std::map<std::string, fs::path> loaded_packages_;
    std::vector<fs::path> stack_;
    std::unordered_set<std::string> loaded_standard_declarations_;

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
            manifest = try_read_package_manifest(normalized.parent_path());
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
                            resolve_installed_package_path(dependency_name);
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
                                dependency_main->parent_path());
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
                    const auto descriptor = read_text(descriptor_path);
                    const auto document = parse_toml_subset(
                        descriptor, descriptor_path.string());
                    const auto* extension_version =
                        document.find("extension", "version");
                    const auto* phase =
                        document.find("extension", "phase");
                    if (!extension_version || *extension_version != "1") {
                        throw std::runtime_error(
                            "compiler extension descriptor requires "
                            "[extension] version = 1: " +
                            descriptor_path.string());
                    }
                    if (!phase || *phase != "tensor-region") {
                        throw std::runtime_error(
                            "compiler extension descriptor has unsupported "
                            "phase; expected 'tensor-region': " +
                            descriptor_path.string());
                    }
                    std::unordered_set<std::string> operation_ids;
                    for (const auto& [table_name, fields] :
                         document.tables) {
                        if (!table_name.starts_with("operation.")) continue;
                        const auto operation_id = table_name.substr(
                            std::string("operation.").size());
                        if (operation_id.empty()) {
                            throw std::runtime_error(
                                "compiler extension operation table requires "
                                "a non-empty id: " +
                                descriptor_path.string());
                        }
                        const auto function = fields.find("function");
                        if (function == fields.end() ||
                            function->second.empty()) {
                            throw std::runtime_error(
                                "compiler extension operation '" +
                                operation_id +
                                "' requires a non-empty function: " +
                                descriptor_path.string());
                        }
                        operation_ids.insert(operation_id);
                    }

                    std::unordered_set<std::string> execution_policy_ids;
                    for (const auto& [table_name, fields] :
                         document.tables) {
                        if (!table_name.starts_with(
                                "execution_policy.")) {
                            continue;
                        }
                        const auto policy_id = table_name.substr(
                            std::string("execution_policy.").size());
                        const auto function = fields.find("function");
                        if (policy_id.empty() ||
                            function == fields.end() ||
                            function->second.empty()) {
                            throw std::runtime_error(
                                "compiler extension execution policy table "
                                "requires a non-empty id and function: " +
                                descriptor_path.string());
                        }
                        execution_policy_ids.insert(policy_id);
                    }

                    for (const auto& [table_name, fields] :
                         document.tables) {
                        if (!table_name.starts_with("fusion.")) continue;
                        const auto fusion_id = table_name.substr(
                            std::string("fusion.").size());
                        const auto configured = fields.find("operations");
                        if (fusion_id.empty() ||
                            configured == fields.end() ||
                            configured->second.empty()) {
                            throw std::runtime_error(
                                "compiler extension fusion table requires "
                                "a non-empty id and operations list: " +
                                descriptor_path.string());
                        }

                        std::size_t start = 0;
                        while (start <= configured->second.size()) {
                            const auto comma =
                                configured->second.find(',', start);
                            const auto end =
                                comma == std::string::npos
                                    ? configured->second.size()
                                    : comma;
                            auto operation = configured->second.substr(
                                start, end - start);
                            const auto first =
                                operation.find_first_not_of(" \t\r");
                            const auto last =
                                operation.find_last_not_of(" \t\r");
                            if (first == std::string::npos) {
                                throw std::runtime_error(
                                    "compiler extension fusion '" +
                                    fusion_id +
                                    "' contains an empty operation id: " +
                                    descriptor_path.string());
                            }
                            operation =
                                operation.substr(first, last - first + 1);
                            if (!operation_ids.contains(operation)) {
                                throw std::runtime_error(
                                    "compiler extension fusion '" +
                                    fusion_id +
                                    "' references unknown operation '" +
                                    operation + "': " +
                                    descriptor_path.string());
                            }
                            if (comma == std::string::npos) break;
                            start = comma + 1;
                        }

                        if (const auto replacement =
                                fields.find("replacement");
                            replacement != fields.end()) {
                            if (replacement->second.empty() ||
                                !operation_ids.contains(replacement->second)) {
                                throw std::runtime_error(
                                    "compiler extension fusion '" +
                                    fusion_id +
                                    "' references unknown replacement operation '" +
                                    replacement->second + "': " +
                                    descriptor_path.string());
                            }
                        }
                    }
                    for (const auto& [table_name, fields] :
                         document.tables) {
                        std::string rule_kind;
                        std::string rule_prefix;
                        if (table_name.starts_with("specialization.")) {
                            rule_kind = "specialization";
                            rule_prefix = "specialization.";
                        } else if (table_name.starts_with("backend.")) {
                            rule_kind = "backend";
                            rule_prefix = "backend.";
                        } else if (table_name.starts_with("memory.")) {
                            rule_kind = "memory";
                            rule_prefix = "memory.";
                        } else {
                            continue;
                        }

                        const auto rule_id =
                            table_name.substr(rule_prefix.size());
                        const auto operation = fields.find("operation");
                        const auto replacement = fields.find("replacement");
                        if (rule_id.empty() ||
                            operation == fields.end() ||
                            operation->second.empty() ||
                            replacement == fields.end() ||
                            replacement->second.empty()) {
                            throw std::runtime_error(
                                "compiler extension " + rule_kind +
                                " table requires a non-empty id, operation, "
                                "and replacement: " +
                                descriptor_path.string());
                        }
                        if (!operation_ids.contains(operation->second)) {
                            throw std::runtime_error(
                                "compiler extension " + rule_kind + " '" +
                                rule_id +
                                "' references unknown operation '" +
                                operation->second + "': " +
                                descriptor_path.string());
                        }
                        if (!operation_ids.contains(replacement->second)) {
                            throw std::runtime_error(
                                "compiler extension " + rule_kind + " '" +
                                rule_id +
                                "' references unknown replacement operation '" +
                                replacement->second + "': " +
                                descriptor_path.string());
                        }
                        if (const auto policy = fields.find("policy");
                            policy != fields.end()) {
                            if (policy->second.empty() ||
                                !execution_policy_ids.contains(
                                    policy->second)) {
                                throw std::runtime_error(
                                    "compiler extension " + rule_kind + " '" +
                                    rule_id +
                                    "' references unknown execution policy '" +
                                    policy->second + "': " +
                                    descriptor_path.string());
                            }
                        }
                    }
                    compiler_extensions_->push_back(
                        CompilerExtensionRegistration{
                            name,
                            extension_name,
                            normalized.parent_path().string(),
                            descriptor_path.string(),
                            descriptor,
                            *phase,
                            document.tables});
                }
            } catch (const std::exception& error) {
                frontend_error(
                    "PACKAGE_COMPILER_EXTENSION", error.what(), span);
            }
        }

        if (!enforce_package_lock_) return;

        if (!package_lock_loaded_) {
            try {
                package_lock_ = read_package_lock(cwd_);
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

    Exports load_file(const fs::path& source_path, const std::string& ns, bool root, Program& merged) {
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
                source = read_text(absolute);
            } catch (const std::exception&) {
                frontend_error("IMPORT_IO", "Cannot read imported module '" + absolute.string() + "'.");
            }
        }

        const auto source_file = absolute.string();
        merged.source_texts[source_file] = source;

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
        resolve_local_prototypes(program, source);
        SourceOrderValidator(program).run();
        for (auto& function : program.functions) function.source_file = source_file;
        for (auto& declaration : program.enums) declaration.source_file = source_file;
        for (auto& declaration : program.classes) {
            declaration.source_file = source_file;
            for (auto& method : declaration.methods) method.source_file = source_file;
        }

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

        Exports exports;
        for (const auto& class_decl : program.classes)
            insert_class_export(
                exports, class_decl.name, qualify(ns, class_decl.name), class_decl.span);
        for (const auto& enum_decl : program.enums)
            exports.enums[enum_decl.name] = qualify(ns, enum_decl.name);
        for (const auto& name : local_functions) exports.functions[name] = qualify(ns, name);
        if (!root) {
            for (const auto& statement : program.statements) {
                const auto* binding = std::get_if<BindingStmt>(&statement->data);
                if (!binding || !binding->is_const || !binding->value) continue;
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
                auto value = clone_package_constant_expression(
                    *binding->value, binding->value->span);
                if (binding->declared_type.name != "auto") {
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
                    package_path = resolve_installed_package_path(import_decl.target);
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
                auto child_exports = load_file(*package_path, child_ns, false, merged);
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

            const auto child_ns = qualify(ns, import_decl.alias);
            auto child_exports = load_file(target, child_ns, false, merged);
            if (import_decl.is_public) {
                exports.namespaces[import_decl.alias] = std::make_shared<Exports>(child_exports);
            }
            imports.emplace(import_decl.alias, ImportBinding{child_ns, std::move(child_exports)});
        }
        stack_.pop_back();

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
            }
            class_decl.name = qualify(ns, class_decl.name);
        }

        for (auto& function : program.functions) {
            rename_function(function, ns, local_classes, local_functions, imports);
            function.name = qualify(ns, function.name);
        }

        for (auto& statement : program.statements) {
            if (root || std::holds_alternative<MainGuardStmt>(statement->data))
                rename_stmt(*statement, ns, local_classes, local_functions, imports, {});
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
    out.tensor_shape_expressions = source.tensor_shape_expressions;
    out.tensor_rank = source.tensor_rank;
    out.tensor_known_shape_prefix = source.tensor_known_shape_prefix;
    for (const auto& argument : source.arguments) out.arguments.push_back(clone_type(argument));
    for (const auto& parameter : source.function_parameters)
        out.function_parameters.push_back(clone_type(parameter));
    return out;
}

std::string canonical_extent_expression(const Expr& expression) {
    if (const auto* literal = std::get_if<IntegerExpr>(&expression.data)) {
        return std::to_string(literal->value);
    }
    if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
        return name->name;
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&expression.data)) {
        return unary->op + "(" + canonical_extent_expression(*unary->operand) + ")";
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
        return "(" + canonical_extent_expression(*binary->left) + binary->op +
               canonical_extent_expression(*binary->right) + ")";
    }
    return "<extent>";
}

std::string canonical_type(const TypeName& type) {
    std::string out = type.name;
    if (!type.arguments.empty()) {
        out += "<";
        for (std::size_t i = 0; i < type.arguments.size(); ++i) {
            if (i) out += ",";
            out += canonical_type(type.arguments[i]);
        }
        out += ">";
    }
    if (type.name == "fn") {
        out += "(";
        for (std::size_t i = 0; i < type.function_parameters.size(); ++i) {
            if (i) out += ",";
            out += canonical_type(type.function_parameters[i]);
        }
        out += ")";
    }
    if (!type.tensor_shape_prefix.empty()) {
        out += "<";
        for (std::size_t i = 0; i < type.tensor_shape_prefix.size(); ++i) {
            if (i) out += ",";
            if (i < type.tensor_shape_expressions.size() &&
                type.tensor_shape_expressions[i]) {
                out += canonical_extent_expression(*type.tensor_shape_expressions[i]);
            } else {
                const auto extent = type.tensor_shape_prefix[i];
                out += extent < 0 ? "_" : std::to_string(extent);
            }
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

std::string safe_name(std::string name) {
    for (auto& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    return name;
}

bool contains_parameter(const TypeName& type, const std::unordered_set<std::string>& parameters) {
    if (parameters.contains(type.name)) return true;
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       [&](const auto& argument) { return contains_parameter(argument, parameters); }) ||
           std::any_of(type.function_parameters.begin(), type.function_parameters.end(),
                       [&](const auto& parameter) { return contains_parameter(parameter, parameters); });
}

bool standard_collection_key_type(const TypeName& type) {
    if (!type.arguments.empty() || !type.dimensions.empty()) return false;
    return type.name == "int" || type.name == "int8" || type.name == "int16" ||
           type.name == "int32" || type.name == "uint8" || type.name == "uint16" ||
           type.name == "uint32" || type.name == "uint64" || type.name == "bigint" ||
           type.name == "bool" ||
           type.name == "string";
}

std::optional<std::vector<StmtPtr>> standard_collection_hash_body(
    const TypeName& type,
    const std::string& parameter) {
    if (!type.arguments.empty() || !type.dimensions.empty()) return std::nullopt;

    std::ostringstream source;
    if (type.name == "bool") {
        source << "int __hash(bool " << parameter << ")\n"
               << "    if " << parameter << "\n"
               << "        return 1\n"
               << "    return 0\n";
    } else if (
        type.name == "int" || type.name == "int8" ||
        type.name == "int16" || type.name == "int32") {
        source << "int __hash(" << type.name << " " << parameter << ")\n"
               << "    int __value = int(" << parameter << ")\n"
               << "    return __value AND 9223372036854775807\n";
    } else if (
        type.name == "uint8" || type.name == "uint16" ||
        type.name == "uint32") {
        source << "int __hash(" << type.name << " " << parameter << ")\n"
               << "    return int(" << parameter << ")\n";
    } else if (type.name == "uint64") {
        source << "int __hash(uint64 " << parameter << ")\n"
               << "    uint64 __result = " << parameter
               << " AND uint64(9223372036854775807)\n"
               << "    return int(__result)\n";
    } else if (type.name == "bigint") {
        source << "int __hash(bigint " << parameter << ")\n"
               << "    bigint __result = " << parameter << " % bigint(2147483647)\n"
               << "    if __result < 0\n"
               << "        __result += 2147483647\n"
               << "    return int(__result)\n";
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
        output_.source_texts = source_.source_texts;
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
            function_return_types_[name] = materialize_type(declaration->return_type, {}, {});
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
        return name == "int" || name == "int64" || name == "int8" ||
               name == "int16" || name == "int32" || name == "uint8" ||
               name == "uint16" || name == "uint32" || name == "uint64" ||
               name == "bigint";
    }

    static bool scalar_floating_constraint_type(std::string_view name) {
        return name == "float" || name == "float64" || name == "float32";
    }

    static bool scalar_numeric_constraint_type(std::string_view name) {
        return scalar_integer_constraint_type(name) ||
               scalar_floating_constraint_type(name) || name == "bigreal";
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
        if (!type.arguments.empty() || !type.function_parameters.empty() ||
            type.name == "tensor" || type.name == "fn" ||
            type.name == "union") {
            return false;
        }
        if (scalar_numeric_constraint_type(type.name) || type.name == "bool" ||
            type.name == "string" || type.name == "bin" || type.name == "error") {
            return true;
        }
        if (type.name == "$std.json.Value" ||
            type.name == "$std.http.Response" ||
            type.name == "$std.file.Handle" ||
            type.name == "$std.atomic.Counter") {
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
                scalar_numeric_constraint_type(type.name);
        if (constraint == "equatable") {
            std::unordered_set<std::string> visiting;
            return equatable_constraint_type(std::move(type), false, visiting);
        }
        return false;
    }

    void validate_type_parameters(
        const std::vector<std::string>& parameters,
        const std::vector<std::string>& constraints,
        const std::unordered_set<std::string>& additionally_reserved,
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
                additionally_reserved.contains(parameter) || parameter == "main") {
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

    // `construct(...)` names the one constructor a class may declare. The
    // bare spelling produces the enclosing class; `T | error construct(...)`
    // is the one spelled form for a constructor that can fail. Optional call
    // shapes belong in default parameters rather than constructor overloads.
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
        if (method.constructor_typed && declared == self_spelling) {
            frontend_error("CONSTRUCTOR_SIGNATURE",
                           "construct does not name its own type; write construct(...) for a constructor of '" +
                               class_decl.name + "', or '" + self_spelling +
                               " | error construct(...)' for one that can fail.",
                           method.span);
        }
        if (method.constructor_typed && declared != canonical_type(fallible)) {
            frontend_error("CONSTRUCTOR_SIGNATURE",
                           "A constructor of '" + class_decl.name + "' is spelled construct(...) or '" +
                               self_spelling + " | error construct(...)'; it cannot return '" + declared + "'.",
                           method.span);
        }
        if (!method.type_parameters.empty()) {
            frontend_error("CONSTRUCTOR_SIGNATURE",
                           "Constructors take the class's type parameters and cannot declare their own.",
                           method.span);
        }
    }

    std::string generic_parameter_shape(
        const TypeName& source,
        const std::vector<std::string>& parameters) const {
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
        return canonical_type(type);
    }

    std::string generic_call_shape(const FunctionDecl& function) const {
        std::string key;
        for (const auto& parameter : function.parameters) {
            if (!key.empty()) key += ";";
            if (parameter.is_const) key += "const ";
            key += generic_parameter_shape(parameter.type, function.type_parameters);
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
        for (std::size_t i = 0; i < generic.parameters.size(); ++i) {
            const auto& pattern = generic.parameters[i].type;
            const auto& actual = concrete.parameters[i].type;
            if (!infer_generic_pattern(pattern, actual, parameters, inferred)) {
                return false;
            }
            if (canonical_type(substitute_raw(pattern, inferred)) !=
                canonical_type(actual)) {
                return false;
            }
        }
        return true;
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
        if (left.type_parameters.size() != right.type_parameters.size()) return false;
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
        for (std::size_t i = 0; i < left.type_parameters.size(); ++i) {
            if (constraints_disjoint(
                    function_constraint(left, i), function_constraint(right, i))) {
                return true;
            }
        }
        return false;
    }

    std::string concrete_signature_key(const FunctionDecl& function) const {
        std::string key;
        for (const auto& parameter : function.parameters) {
            if (!key.empty()) key += ";";
            if (parameter.is_const) key += "const ";
            key += canonical_type(parameter.type);
            if (parameter.writable) key += "&";
            key += " " + parameter.name;
        }
        return key;
    }

    std::string function_declaration_key(const FunctionDecl& function) const {
        std::string key = function.name + "(" + generic_call_shape(function) + ")";
        key += "<";
        for (std::size_t i = 0; i < function.type_parameters.size(); ++i) {
            if (i) key += ",";
            key += std::string(function_constraint(function, i));
        }
        key += ">->" + generic_parameter_shape(
            function.return_type, function.type_parameters);
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
            for (std::size_t i = 0; i < arity; ++i) {
                if (function->parameters[i].name != reference.parameters[i].name ||
                    function->parameters[i].writable != reference.parameters[i].writable ||
                    function->parameters[i].is_const != reference.parameters[i].is_const ||
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

        const auto generic_shape = generic_call_shape(*generics.front());
        for (const auto* function : generics) {
            if (generic_call_shape(*function) != generic_shape ||
                function->type_parameters.size() != generics.front()->type_parameters.size()) {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Generic specializations of '" + name +
                        "' must keep one generic call shape; ordinary overloads are not supported.",
                    function->span);
            }
        }

        for (const auto* function : concretes) {
            if (!concrete_matches_generic_pattern(*generics.front(), *function)) {
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
                declaration_names, class_decl.span);
            const std::unordered_set<std::string> class_parameters{
                class_decl.type_parameters.begin(), class_decl.type_parameters.end()};
            const bool standard_generated =
                class_decl.name.rfind("$std.", 0) == 0 ||
                class_decl.name.rfind("__quidra_gc__std_", 0) == 0;
            // Imported package classes are namespace-scoped. Their members are
            // never introduced as bare names in the importing program, so a
            // package can preserve natural qualified member APIs
            // without weakening reservation for root user declarations.
            const bool namespace_scoped =
                standard_generated ||
                class_decl.name.find('.') != std::string::npos;
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
                                "' declares more than one construct(...); use default parameters on a single constructor.",
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
                    class_parameters, method.span);
            }
            for (const auto& [name, family] : method_families) {
                if (family.size() > 1) validate_function_family(name, family);
            }
        }

        for (const auto& function : source_.functions) {
            validate_type_parameters(
                function.type_parameters, function.type_constraints,
                declaration_names, function.span);
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

    std::string mangle(const std::string& prefix, const std::string& name, const std::string& key) const {
        return "__quidra_" + prefix + "_" + safe_name(name) + "_" + short_hash(key);
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
        signature.span = source.span;
        signature.is_private = source.is_private;
        signature.type_parameters = source.type_parameters;
        signature.type_constraints = source.type_constraints;
        const auto deferred = set_of(source.type_parameters);
        signature.return_type =
            materialize_type(source.return_type, class_substitution, deferred);
        for (const auto& parameter : source.parameters) {
            Parameter copy;
            copy.name = parameter.name;
            copy.type =
                materialize_type(parameter.type, class_substitution, deferred);
            copy.writable = parameter.writable;
            copy.span = parameter.span;
            copy.is_const = parameter.is_const;
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
            std::holds_alternative<FloatExpr>(expression.data)) {
            return std::nullopt;
        }
        if (std::holds_alternative<StringExpr>(expression.data) ||
            std::holds_alternative<StringTemplateExpr>(expression.data)) return simple_type("string");
        if (std::holds_alternative<BoolExpr>(expression.data)) return simple_type("bool");

        if (const auto* name = std::get_if<NameExpr>(&expression.data)) {
            if (const auto local = type_environment_.find(name->name);
                local != type_environment_.end()) {
                return clone_type(local->second);
            }
            if (!current_class.empty()) {
                if (const auto* field = output_field(current_class, name->name)) {
                    return clone_type(field->type);
                }
            }
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

        if (const auto* binary = std::get_if<BinaryExpr>(&expression.data)) {
            if (binary->op != "+" && binary->op != "-" &&
                binary->op != "*" && binary->op != "/" &&
                binary->op != "%" && binary->op != "^") {
                return std::nullopt;
            }
            auto left = infer_expression_type(*binary->left, current_class);
            auto right = infer_expression_type(*binary->right, current_class);
            const auto numeric_literal = [](const Expr& value) {
                return std::holds_alternative<IntegerExpr>(value.data) ||
                       std::holds_alternative<FloatExpr>(value.data);
            };
            const auto with_span = [&](TypeName value) {
                value.span = expression.span;
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
                return std::nullopt;
            }
            if (left && numeric_literal(*binary->right))
                return with_span(clone_type(*left));
            if (right && numeric_literal(*binary->left))
                return with_span(clone_type(*right));
            return std::nullopt;
        }

        if (const auto* call = std::get_if<CallExpr>(&expression.data)) {
            if ((call->callee == "tensor" || call->callee == "$std.tensor.zeros" ||
                 call->callee == "$std.tensor.ones") &&
                call->type_arguments.size() == 1 && call->args.size() == 1) {
                TypeName result;
                result.name = "tensor";
                result.arguments.push_back(clone_type(call->type_arguments.front()));
                const auto shape =
                    infer_expression_type(*call->args[0].value, current_class);
                if (shape && shape->name == "int" &&
                    shape->dimensions.size() == 1 &&
                    shape->dimensions.front() >= 0) {
                    result.tensor_rank = shape->dimensions.front();
                }
                result.span = expression.span;
                return result;
            }
            static const std::unordered_map<std::string, std::string>
                numeric_cast_result_types{
                    {"int8", "int8"}, {"int16", "int16"}, {"int32", "int32"},
                    {"int", "int"}, {"int64", "int"},
                    {"uint8", "uint8"}, {"uint16", "uint16"},
                    {"uint32", "uint32"}, {"uint64", "uint64"},
                    {"bigint", "bigint"},
                    {"float32", "float32"}, {"float", "float"}, {"float64", "float"},
                    {"bigreal", "bigreal"}};
            if (const auto scalar = numeric_cast_result_types.find(call->callee);
                scalar != numeric_cast_result_types.end()) {
                return simple_type(scalar->second);
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
            if (base->name == "tensor") {
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
                const auto project_prefix = [&](std::vector<long long>& prefix) {
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
            if (base->dimensions.empty() || index->items.size() != 1 ||
                index->items.front().slice) {
                return std::nullopt;
            }
            base->dimensions.erase(base->dimensions.begin());
            base->array_depth = base->dimensions.size();
            return base;
        }

        if (const auto* call = std::get_if<MethodCallExpr>(&expression.data)) {
            const auto receiver = infer_expression_type(*call->receiver, current_class);
            if (!receiver || !receiver->dimensions.empty()) return std::nullopt;
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
                    result.name = "int";
                    result.dimensions.push_back(receiver->tensor_rank.value_or(-1));
                    result.array_depth = 1;
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "reshape" && call->args.size() == 1) {
                    auto result = clone_type(*receiver);
                    result.tensor_shape_prefix.clear();
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[0].value, current_class);
                    if (shape && shape->name == "int" &&
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
                    result.tensor_known_shape_prefix.clear();
                    result.span = expression.span;
                    return result;
                }
                if (call->method == "gather" && call->args.size() == 2) {
                    auto result = clone_type(*receiver);
                    result.tensor_shape_prefix.clear();
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[1].value, current_class);
                    if (shape && shape->name == "int" &&
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
                    result.tensor_rank.reset();
                    result.tensor_known_shape_prefix.clear();
                    const auto shape =
                        infer_expression_type(*call->args[1].value, current_class);
                    if (shape && shape->name == "int" &&
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
            candidate.array_depth = candidate.dimensions.size();

            if (const auto existing = inferred.find(pattern.name);
                existing != inferred.end()) {
                return canonical_type(existing->second) == canonical_type(candidate);
            }
            inferred.emplace(pattern.name, std::move(candidate));
            return true;
        }

        if (pattern.name != actual.name ||
            pattern.arguments.size() != actual.arguments.size() ||
            pattern.function_parameters.size() != actual.function_parameters.size() ||
            pattern.dimensions.size() != actual.dimensions.size()) {
            return false;
        }
        if (pattern.name == "tensor" &&
            !pattern.tensor_shape_prefix.empty()) {
            const auto rank = pattern.tensor_shape_prefix.size();
            if (actual.tensor_rank &&
                static_cast<std::size_t>(*actual.tensor_rank) != rank) {
                return false;
            }
            for (std::size_t axis = 0; axis < rank; ++axis) {
                const auto required = pattern.tensor_shape_prefix[axis];
                if (required < 0) continue;
                std::optional<long long> actual_extent;
                if (axis < actual.tensor_known_shape_prefix.size()) {
                    actual_extent = actual.tensor_known_shape_prefix[axis];
                } else if (axis < actual.tensor_shape_prefix.size() &&
                           actual.tensor_shape_prefix[axis] >= 0) {
                    actual_extent = actual.tensor_shape_prefix[axis];
                }
                if (actual_extent && *actual_extent != required) return false;
            }
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
        const std::unordered_set<std::string> parameters{
            templ.type_parameters.begin(), templ.type_parameters.end()};
        Substitution inferred;
        std::vector<bool> filled(templ.parameters.size(), false);
        std::size_t positional = 0;

        for (const auto& argument : args) {
            std::size_t index = templ.parameters.size();
            if (argument.name) {
                for (std::size_t i = 0; i < templ.parameters.size(); ++i) {
                    if (templ.parameters[i].name == *argument.name) {
                        index = i;
                        break;
                    }
                }
            } else {
                while (positional < filled.size() && filled[positional]) ++positional;
                index = positional++;
            }
            if (index >= templ.parameters.size() || filled[index]) return std::nullopt;
            filled[index] = true;

            const auto actual = infer_expression_type(*argument.value, current_class);
            if (!actual) continue;
            if (!infer_generic_pattern(
                    templ.parameters[index].type, *actual, parameters, inferred)) {
                return std::nullopt;
            }
        }

        std::vector<TypeName> result;
        result.reserve(templ.type_parameters.size());
        for (const auto& parameter : templ.type_parameters) {
            const auto found = inferred.find(parameter);
            if (found == inferred.end()) return std::nullopt;
            result.push_back(clone_type(found->second));
        }
        return result;
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

    bool concrete_family_match(
        const FunctionDecl& function,
        const std::vector<CallArg>& args,
        const std::string& current_class) const {
        const auto targets = bind_family_arguments(function, args);
        if (!targets) return false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& expected = function.parameters[(*targets)[i]].type;
            const auto actual = infer_expression_type(*args[i].value, current_class);
            if (!actual) {
                const auto contextual_numeric_literal =
                    [&](const auto& self, const Expr& value) -> bool {
                        if (std::holds_alternative<IntegerExpr>(value.data) ||
                            std::holds_alternative<FloatExpr>(value.data)) {
                            return true;
                        }
                        if (const auto* unary =
                                std::get_if<UnaryExpr>(&value.data)) {
                            return (unary->op == "-" || unary->op == "+") &&
                                self(self, *unary->operand);
                        }
                        return false;
                    };
                const bool numeric_literal =
                    contextual_numeric_literal(
                        contextual_numeric_literal, *args[i].value);
                if (!numeric_literal || !expected.dimensions.empty() ||
                    !expected.arguments.empty() || !expected.function_parameters.empty() ||
                    !scalar_numeric_constraint_type(expected.name)) {
                    return false;
                }
                continue;
            }
            const std::unordered_set<std::string> no_parameters;
            Substitution ignored;
            if (!infer_generic_pattern(
                    expected, *actual, no_parameters, ignored)) {
                return false;
            }
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
            materialize_type(function.return_type, {}, {});
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

    std::string resolve_function_family_explicit(
        const std::string& name,
        const std::vector<TypeName>& source_arguments,
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
        const auto* chosen = most_specific_generic(candidates, span, name);
        return instantiate_function(*chosen, arguments);
    }

    TypeName materialize_type(
        const TypeName& source,
        const Substitution& substitution,
        const std::unordered_set<std::string>& deferred) {
        auto type = substitute_raw(source, substitution);

        if (type.name == "union") {
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
            if (type.name == "fn") {
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

        if (const auto* node = std::get_if<IntegerExpr>(&source.data)) out->data = *node;
        else if (const auto* node = std::get_if<FloatExpr>(&source.data)) out->data = *node;
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
            out->data = BinaryExpr{
                node->op,
                clone_expr(*node->left, substitution, deferred, current_class),
                clone_expr(*node->right, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<TryExpr>(&source.data)) {
            out->data = TryExpr{clone_expr(*node->value, substitution, deferred, current_class)};
        } else if (const auto* node = std::get_if<CallExpr>(&source.data)) {
            auto type_arguments = materialize_type_arguments(node->type_arguments, substitution, deferred);
            const bool deferred_call = std::any_of(
                type_arguments.begin(), type_arguments.end(),
                [&](const auto& argument) { return contains_parameter(argument, deferred); });

            CallExpr copy;
            copy.callee = node->callee;
            copy.args = clone_args(node->args, substitution, deferred, current_class);

            if (!type_arguments.empty() && !deferred_call) {
                if (copy.callee == "tensor" || copy.callee == "$std.tensor.zeros" ||
                    copy.callee == "$std.tensor.ones" ||
                    copy.callee == "$std.reflect.collect" ||
                    copy.callee == "$std.reflect.paths") {
                    copy.type_arguments = std::move(type_arguments);
                } else if (class_templates_.contains(copy.callee)) {
                    copy.callee = instantiate_class(copy.callee, type_arguments);
                } else if (current_class.size() &&
                           class_has_method_specialization_family(
                               current_class, copy.callee)) {
                    copy.callee = resolve_method_family_explicit(
                        current_class, copy.callee, type_arguments, source.span);
                } else if (current_class.size() && class_has_generic_method(current_class, copy.callee)) {
                    request_method_for_class(current_class, copy.callee, type_arguments, source.span);
                    copy.callee = method_name(copy.callee, type_arguments);
                } else if (specialization_families_.contains(copy.callee)) {
                    copy.callee = resolve_function_family_explicit(
                        copy.callee, type_arguments, source.span);
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
                } else if (class_templates_.contains(copy.callee)) {
                    frontend_error(
                        "GENERIC_ARGUMENTS_REQUIRED",
                        "Generic class '" + copy.callee +
                            "' requires explicit type arguments.",
                        source.span);
                }
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<MethodCallExpr>(&source.data)) {
            MethodCallExpr copy;
            copy.receiver = clone_expr(*node->receiver, substitution, deferred, current_class);
            copy.method = node->method;
            copy.args = clone_args(node->args, substitution, deferred, current_class);

            auto type_arguments = materialize_type_arguments(node->type_arguments, substitution, deferred);
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
                if (receiver_type->name == "$std.autograd.Target" &&
                    copy.method == "gradient") {
                    // autograd.Target.gradient<T>() is a built-in method. Keep its
                    // explicit type argument for the checker instead of trying to
                    // monomorphize a source-declared generic method.
                    copy.type_arguments = std::move(type_arguments);
                } else if (class_has_method_specialization_family(
                        receiver_type->name, copy.method)) {
                    copy.method = resolve_method_family_explicit(
                        receiver_type->name, copy.method,
                        type_arguments, source.span);
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
            if (node->value) copy.value = clone_expr(*node->value, substitution, deferred, current_class);
            if (copy.declared_type.name != "auto") {
                type_environment_[copy.name] = clone_type(copy.declared_type);
            } else if (copy.value) {
                if (const auto inferred = infer_expression_type(*copy.value, current_class)) {
                    type_environment_[copy.name] = clone_type(*inferred);
                }
            }
            out->data = std::move(copy);
        } else if (const auto* node = std::get_if<AssignStmt>(&source.data)) {
            out->data = AssignStmt{
                clone_expr(*node->target, substitution, deferred, current_class),
                clone_expr(*node->value, substitution, deferred, current_class),
                node->compound_op};
        } else if (const auto* node = std::get_if<RebindStmt>(&source.data)) {
            out->data = RebindStmt{
                node->name, clone_expr(*node->target, substitution, deferred, current_class)};
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
            MainGuardStmt copy;
            copy.active = node->active;
            copy.source_file = node->source_file;
            const auto before = type_environment_;
            for (const auto& child : node->body)
                copy.body.push_back(clone_stmt(*child, substitution, deferred, current_class));
            type_environment_ = before;
            out->data = std::move(copy);
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
            copy.iterable = clone_expr(*node->iterable, substitution, deferred, current_class);
            const auto before = type_environment_;
            if (const auto iterable_type = infer_expression_type(*copy.iterable, current_class);
                iterable_type && !iterable_type->dimensions.empty()) {
                auto element_type = clone_type(*iterable_type);
                element_type.dimensions.erase(element_type.dimensions.begin());
                element_type.array_depth = element_type.dimensions.size();
                type_environment_[copy.name] = std::move(element_type);
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
                case_copy.type = materialize_type(match_case.type, substitution, deferred);
                case_copy.tag = match_case.tag;
                case_copy.binder = match_case.binder;
                case_copy.span = match_case.span;
                if (const auto* name = std::get_if<NameExpr>(&copy.value->data)) {
                    type_environment_[name->name] = clone_type(case_copy.type);
                }
                if (case_copy.binder) {
                    type_environment_[*case_copy.binder] = clone_type(case_copy.type);
                }
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
        const auto saved_environment = type_environment_;
        type_environment_.clear();
        try {
            FunctionDecl out;
            out.name = source.name;
            out.source_file = source.source_file;
            out.return_type = materialize_type(source.return_type, substitution, deferred);
            out.span = source.span;
            out.is_private = source.is_private;
            out.is_constructor = source.is_constructor;
            out.constructor_typed = source.constructor_typed;
            out.type_parameters = source.type_parameters;
            out.external_symbol = source.external_symbol;
            out.type_constraints = source.type_constraints;

            for (const auto& parameter : source.parameters) {
                Parameter copy;
                copy.name = parameter.name;
                copy.type = materialize_type(parameter.type, substitution, deferred);
                copy.writable = parameter.writable;
                copy.span = parameter.span;
                copy.is_const = parameter.is_const;
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
            type_environment_ = saved_environment;
            return out;
        } catch (...) {
            type_environment_ = saved_environment;
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

        if (name == "$std.map.Map" && !arguments.empty() &&
            !standard_collection_key_type(arguments[0])) {
            frontend_error("STANDARD_KEY_TYPE",
                           "map.Map keys must be integer, bool, or string values.",
                           source_arguments[0].span);
        }
        if (name == "$std.set.Set" && !arguments.empty() &&
            !standard_collection_key_type(arguments[0])) {
            frontend_error("STANDARD_KEY_TYPE",
                           "set.Set values must be integer, bool, or string values.",
                           source_arguments[0].span);
        }

        const auto key = instance_key(name, arguments);
        if (const auto existing = class_instances_.find(key); existing != class_instances_.end()) {
            return existing->second;
        }

        const auto concrete_name = mangle("gc", name, key);
        class_instances_[key] = concrete_name;

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

        ClassDecl out;
        out.name = concrete_name;
        out.source_file = source.source_file;
        out.span = source.span;

        for (const auto& field : source.fields) {
            FieldDecl copy;
            copy.name = field.name;
            copy.type = materialize_type(field.type, class_substitution, {});
            copy.span = field.span;
            copy.is_const = field.is_const;
            copy.is_private = field.is_private;
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
                    materialize_type(method.return_type, class_substitution, {});
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
            if ((source.name == "$std.map.Map" || source.name == "$std.set.Set") &&
                method.name == "__hash") {
                const auto parameter = source.name == "$std.map.Map" ? "K" : "T";
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
            materialize_type(function.return_type, substitution, {});
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
            materialize_type(member.source->return_type, combined, {});
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
            materialize_type(templ.return_type, substitution, {});
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
    std::size_t max_errors) {
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    auto program = ModuleLoader(
        command_working_directory, max_errors, std::nullopt, nullptr, true,
        &compiler_extensions)
        .load(root_file);
    return ResolvedProgram{
        std::move(program), std::move(compiler_extensions)};
}

ResolvedProgram load_program_with_root_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors,
    bool enforce_package_lock) {
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    auto program = ModuleLoader(
        command_working_directory, max_errors, std::string(root_source),
        nullptr, enforce_package_lock, &compiler_extensions)
        .load(root_file);
    return ResolvedProgram{
        std::move(program), std::move(compiler_extensions)};
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

ConcreteProgram expand_generics(ResolvedProgram program) {
    return ConcreteProgram{
        GenericExpander(std::move(program.program)).run(),
        std::move(program.compiler_extensions)};
}

} // namespace quidra