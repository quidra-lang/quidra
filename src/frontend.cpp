#include "quidra/frontend.hpp"

#include "quidra/diagnostic.hpp"
#include "quidra/import_path.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
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
    std::unordered_map<std::string, std::string> functions;
    std::unordered_map<std::string, std::string> values;
};

Exports standard_exports(const std::string& module, SourceSpan span) {
    if (!is_standard_module(module)) {
        frontend_error("UNKNOWN_STANDARD_MODULE",
                       "Unknown standard namespace '" + module +
                           "'. Standard namespaces come from the language registry; unquoted non-standard imports resolve installed packages.",
                       span);
    }
    Exports exports;
    if (module == "math") {
        for (const char* name : {"abs", "sqrt", "min", "max", "sin", "cos", "tan", "log", "exp", "pow", "trunc", "round", "floor", "ceil"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
        for (const char* name : {"pi", "e"}) {
            exports.values.emplace(name, std::string(*standard_value_target(module, name)));
        }
    } else if (module == "file") {
        for (const char* name : {"read", "write", "read_bin", "write_bin", "exists", "is_directory", "remove", "copy", "move", "mkdir", "list"}) {
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
    } else if (module == "task") {
        exports.functions.emplace("all", std::string(*standard_function_target(module, "all")));
    } else if (module == "random") {
        exports.classes.emplace("Generator", "$std.random.Generator");
        exports.functions.emplace("generator", std::string(*standard_function_target(module, "generator")));
    } else if (module == "process") {
        exports.classes.emplace("Result", "$std.process.Result");
        for (const char* name : {"run", "exit"}) {
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
    } else if (module == "stats") {
        for (const auto name : {"sum", "mean", "min", "max"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "linear") {
        exports.functions.emplace("dot", std::string(*standard_function_target(module, "dot")));
        exports.functions.emplace("matmul", std::string(*standard_function_target(module, "matmul")));
    } else if (module == "image") {
        for (const char* name : {
                 "read", "write", "tensor_crop", "tensor_resize",
                 "tensor_flip_horizontal", "tensor_flip_vertical",
                 "tensor_rotate90", "tensor_rotate270", "tensor_grayscale",
                 "tensor_threshold", "tensor_blur", "tensor_filter",
                 "tensor_dilate", "tensor_erode"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
    } else if (module == "neural") {
        exports.classes.emplace("Gradients", "$std.neural.Gradients");
        exports.classes.emplace("Parameter", "$std.neural.Parameter");
        exports.classes.emplace("State", "$std.neural.State");
        for (const char* name : {"track", "affine", "convolve2d", "absolute", "exponential",
                                 "logarithm", "mean", "sum_last", "max_last", "update", "normalize",
                                 "normalize_inference", "random_mask", "moment_update", "grad", "save", "load"}) {
            exports.functions.emplace(name, std::string(*standard_function_target(module, name)));
        }
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
    if (module == "time") {
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
    } else if (module == "neural") {
        TypeName generic_t = standard_type("T");
        TypeName tensor_t = standard_type("tensor");
        tensor_t.arguments.push_back(generic_t);
        TypeName neural_t = standard_type("neural");
        neural_t.arguments.push_back(generic_t);

        ClassDecl parameter;
        parameter.name = "$std.neural.Parameter";
        parameter.span = standard_span();
        parameter.type_parameters.push_back("T");
        FieldDecl parameter_value;
        parameter_value.name = "value";
        parameter_value.type = tensor_t;
        parameter_value.span = standard_span();
        parameter.fields.push_back(std::move(parameter_value));
        std::vector<CallArg> parameter_track_args;
        parameter_track_args.push_back(standard_arg(standard_name("value")));
        parameter.methods.push_back(standard_method(
            "track", std::vector<Parameter>{}, neural_t,
            standard_call(
                "$std.neural.parameter_track",
                std::move(parameter_track_args))));
        parameter.methods.push_back(standard_method(
            "raw", std::vector<Parameter>{}, tensor_t, standard_name("value")));
        declarations.push_back(std::move(parameter));

        ClassDecl state;
        state.name = "$std.neural.State";
        state.span = standard_span();
        state.type_parameters.push_back("T");
        FieldDecl state_value;
        state_value.name = "value";
        state_value.type = generic_t;
        state_value.span = standard_span();
        state.fields.push_back(std::move(state_value));
        declarations.push_back(std::move(state));

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
            if __index >= 0 and __active[__index] and __hashes[__index] == hash and __keys[__index] == key
                __empty_slot = __slot
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
        return void

    void __compact()
        K[] __new_keys = []
        V[] __new_values = []
        int[] __new_hashes = []
        bool[] __new_active = []
        for __index in range(len(__keys))
            if __active[__index]
                __new_keys = __new_keys.append(__keys[__index])
                __new_values = __new_values.append(__values[__index])
                __new_hashes = __new_hashes.append(__hashes[__index])
                __new_active = __new_active.append(true)
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
            if __active[__cached] and __hashes[__cached] == __last_hash and __keys[__cached] == key
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
        __version += 1
        __last_version = -1
        if len(__keys) > 64 and __size * 2 <= len(__keys)
            __compact()
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
            if __index >= 0 and __active[__index] and __hashes[__index] == hash and __values[__index] == value
                __empty_slot = __slot
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
        return void

    void __compact()
        T[] __new_values = []
        int[] __new_hashes = []
        bool[] __new_active = []
        for __index in range(len(__values))
            if __active[__index]
                __new_values = __new_values.append(__values[__index])
                __new_hashes = __new_hashes.append(__hashes[__index])
                __new_active = __new_active.append(true)
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
        if len(__values) > 64 and __size * 2 <= len(__values)
            __compact()
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
        const auto head = first_segment(type.name);
        if (const auto it = imports.find(head); it != imports.end()) {
            const auto suffix = suffix_after_first(type.name);
            if (suffix.empty()) {
                frontend_error("UNKNOWN_TYPE", "Imported module alias cannot be used as a type.", type.span);
            }
            if (!it->second.exports.classes.contains(suffix)) {
                frontend_error("UNKNOWN_TYPE",
                               "Module '" + head + "' has no exported class '" + suffix + "'.",
                               type.span);
            }
            type.name = it->second.exports.classes.at(suffix);
        }
        return;
    }

    if (local_classes.contains(type.name)) {
        type.name = qualify(ns, type.name);
    }
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
        if (const auto* base = std::get_if<NameExpr>(&node->base->data)) {
            if (const auto import = imports.find(base->name); import != imports.end()) {
                if (const auto value = import->second.exports.values.find(node->name);
                    value != import->second.exports.values.end()) {
                    expression.data = NameExpr{value->second};
                    return;
                }
                if (const auto function = import->second.exports.functions.find(node->name);
                    function != import->second.exports.functions.end()) {
                    expression.data = NameExpr{function->second};
                    return;
                }
            }
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
        const auto* receiver_name = std::get_if<NameExpr>(&node->receiver->data);
        if (receiver_name) {
            if (const auto import = imports.find(receiver_name->name); import != imports.end()) {
                for (auto& type_argument : node->type_arguments) {
                    rename_type(type_argument, ns, local_classes, imports, type_parameters);
                }
                rename_call_args(node->args, ns, local_classes, local_functions, imports, type_parameters);

                std::string callee;
                if (const auto fn = import->second.exports.functions.find(node->method);
                    fn != import->second.exports.functions.end()) {
                    callee = fn->second;
                } else if (const auto cls = import->second.exports.classes.find(node->method);
                           cls != import->second.exports.classes.end()) {
                    callee = cls->second;
                } else {
                    frontend_error("UNKNOWN_MODULE_MEMBER",
                                   "Module '" + receiver_name->name + "' has no exported callable '" +
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

class ModuleLoader {
public:
    ModuleLoader(
        fs::path cwd, std::size_t max_errors,
        std::optional<std::string> root_source = std::nullopt,
        std::map<std::string, fs::path>* resolved_packages = nullptr,
        bool enforce_package_lock = true)
        : cwd_(fs::absolute(std::move(cwd)).lexically_normal()),
          max_errors_(max_errors ? max_errors : 1),
          root_source_(std::move(root_source)),
          resolved_packages_(resolved_packages),
          enforce_package_lock_(enforce_package_lock) {}

    Program load(const fs::path& root) {
        Program merged;
        merged.language_version = std::string(language_version);
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
                            "' is recorded in quidra.lock but is not reached by the current import graph. "
                            "Regenerate it with 'quidra lock FILE.qui'.");
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
                        parse_semantic_version(compiler_version))) {
                    frontend_error(
                        "PACKAGE_COMPATIBILITY",
                        "Package '" + name + "' " + manifest->version.str() +
                            " requires Quidra " + requirement->second.text +
                            "; current compiler is " +
                            std::string(compiler_version) + ".",
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
                    "' is imported but is not recorded in quidra.lock. "
                    "Regenerate it with 'quidra lock FILE.qui'.",
                span);
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
        if (relative.extension().empty()) relative += ".qui";
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
        const auto source_file = absolute.string();
        for (auto& function : program.functions) function.source_file = source_file;
        for (auto& declaration : program.classes) {
            declaration.source_file = source_file;
            for (auto& method : declaration.methods) method.source_file = source_file;
        }

        for (const auto& class_decl : program.classes) {
            if (class_decl.name.rfind("$cli.", 0) == 0) continue;
            if (is_reserved_value_name(class_decl.name) || class_decl.name == "main") {
                frontend_error(
                    "DUPLICATE_NAME",
                    "Class name '" + class_decl.name + "' is reserved.",
                    class_decl.span);
            }
        }
        for (const auto& function : program.functions) {
            if (is_reserved_value_name(function.name) || function.name == "main") {
                frontend_error(
                    function.name == "main" ? "RESERVED_MAIN" : "DUPLICATE_NAME",
                    "Function name '" + function.name + "' is reserved.",
                    function.span);
            }
        }

        if (!root && !program.statements.empty()) {
            frontend_error("IMPORT_TOP_LEVEL",
                           "Imported modules may contain declarations only; top-level executable statements belong in the root file.",
                           program.statements.front()->span);
        }

        std::unordered_set<std::string> local_classes;
        std::unordered_set<std::string> local_functions;
        for (const auto& class_decl : program.classes) local_classes.insert(class_decl.name);
        for (const auto& function : program.functions) local_functions.insert(function.name);

        Exports exports;
        for (const auto& name : local_classes) exports.classes[name] = qualify(ns, name);
        for (const auto& name : local_functions) exports.functions[name] = qualify(ns, name);

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
            if (is_reserved_value_name(import_decl.alias) || import_decl.alias == "main") {
                stack_.pop_back();
                frontend_error(
                    "SHADOWING",
                    "Import alias '" + import_decl.alias + "' shadows a reserved visible name.",
                    import_decl.span);
            }
            if (!aliases.insert(import_decl.alias).second ||
                local_classes.contains(import_decl.alias) ||
                local_functions.contains(import_decl.alias)) {
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
            imports.emplace(import_decl.alias, ImportBinding{child_ns, std::move(child_exports)});
        }
        stack_.pop_back();

        reject_import_alias_shadowing(program, aliases);

        for (auto& class_decl : program.classes) {
            std::unordered_set<std::string> class_parameters(
                class_decl.type_parameters.begin(), class_decl.type_parameters.end());

            if (class_decl.parent_type) {
                rename_type(*class_decl.parent_type, ns, local_classes, imports, class_parameters);
                class_decl.parent = class_decl.parent_type->name;
            } else if (class_decl.parent) {
                TypeName parent;
                parent.name = *class_decl.parent;
                parent.span = class_decl.span;
                rename_type(parent, ns, local_classes, imports, class_parameters);
                class_decl.parent = parent.name;
            }

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

        if (root) {
            for (auto& statement : program.statements) {
                rename_stmt(*statement, ns, local_classes, local_functions, imports, {});
            }
        } else {
            // Keep imported declarations in the semantic program while marking their top-level
            // spans as non-root. Source inspection/patching can then remain root-file scoped.
            const auto imported_offset = std::numeric_limits<std::size_t>::max();
            for (auto& class_decl : program.classes) {
                class_decl.span.start.offset = imported_offset;
                class_decl.span.end.offset = imported_offset;
            }
            for (auto& function : program.functions) {
                function.span.start.offset = imported_offset;
                function.span.end.offset = imported_offset;
            }
        }

        for (auto& class_decl : program.classes) merged.classes.push_back(std::move(class_decl));
        for (auto& function : program.functions) merged.functions.push_back(std::move(function));
        if (root) {
            for (auto& statement : program.statements) merged.statements.push_back(std::move(statement));
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
        output_.language_version = source_.language_version;
        output_.root_source_file = source_.root_source_file;
        validate_declarations();

        for (auto& class_decl : source_.classes) {
            if (class_decl.type_parameters.empty()) {
                concrete_classes_[class_decl.name] = &class_decl;
            } else {
                class_templates_[class_decl.name] = &class_decl;
            }
        }
        for (auto& function : source_.functions) {
            if (function.type_parameters.empty()) {
                concrete_functions_[function.name] = &function;
            } else {
                function_templates_[function.name] = &function;
            }
        }
    }

    Program run() {
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
    void validate_type_parameters(
        const std::vector<std::string>& parameters,
        const std::unordered_set<std::string>& additionally_reserved,
        SourceSpan span) const {
        std::unordered_set<std::string> seen;
        for (const auto& parameter : parameters) {
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
        }
    }

    void validate_declarations() const {
        std::unordered_set<std::string> declaration_names;
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
        for (const auto& function : source_.functions) {
            if (function.name == "main") {
                frontend_error("RESERVED_MAIN",
                               "'main' is reserved for the compiler-generated native entrypoint.",
                               function.span);
            }
            if (is_language_type_name(function.name) || is_reserved_value_name(function.name) ||
                !declaration_names.insert(function.name).second) {
                frontend_error("DUPLICATE_NAME",
                               "Function name '" + function.name + "' is reserved or duplicated.",
                               function.span);
            }
        }

        for (const auto& class_decl : source_.classes) {
            validate_type_parameters(class_decl.type_parameters, declaration_names, class_decl.span);
            const std::unordered_set<std::string> class_parameters{
                class_decl.type_parameters.begin(), class_decl.type_parameters.end()};
            const bool standard_generated =
                class_decl.name.rfind("$std.", 0) == 0 ||
                class_decl.name.rfind("__quidra_gc__std_", 0) == 0;
            std::unordered_set<std::string> member_names;
            for (const auto& field : class_decl.fields) {
                if (!standard_generated && is_reserved_value_name(field.name)) {
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
            for (const auto& method : class_decl.methods) {
                if (!standard_generated && is_reserved_value_name(method.name)) {
                    frontend_error("SHADOWING",
                                   "Class method name '" + method.name + "' is reserved.",
                                   method.span);
                }
                if (!member_names.insert(method.name).second) {
                    frontend_error("DUPLICATE_NAME",
                                   "Duplicate class member '" + method.name + "'.",
                                   method.span);
                }
                validate_type_parameters(method.type_parameters, class_parameters, method.span);
            }
        }

        for (const auto& function : source_.functions) {
            validate_type_parameters(function.type_parameters, declaration_names, function.span);
        }
    }

    Program source_;
    Program output_;
    std::unordered_map<std::string, ClassDecl*> class_templates_;
    std::unordered_map<std::string, ClassDecl*> concrete_classes_;
    std::unordered_map<std::string, FunctionDecl*> function_templates_;
    std::unordered_map<std::string, FunctionDecl*> concrete_functions_;

    std::unordered_map<std::string, std::string> class_instances_;
    std::unordered_map<std::string, std::string> function_instances_;
    std::unordered_map<std::string, std::size_t> class_index_;
    std::unordered_map<std::string, std::optional<std::string>> class_parent_;
    std::unordered_set<std::string> building_classes_;

    std::unordered_map<std::string, std::unordered_map<std::string, FunctionDecl>> generic_methods_;
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
        std::string current = class_name;
        std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            if (const auto it = generic_methods_.find(current);
                it != generic_methods_.end() && it->second.contains(method)) {
                return true;
            }
            const auto parent = class_parent_.find(current);
            if (parent == class_parent_.end() || !parent->second) break;
            current = *parent->second;
        }
        return false;
    }

    std::optional<std::string> generic_method_owner(
        const std::string& class_name,
        const std::string& method) const {
        std::string current = class_name;
        std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            if (const auto methods = generic_methods_.find(current);
                methods != generic_methods_.end() && methods->second.contains(method)) {
                return current;
            }
            const auto parent = class_parent_.find(current);
            if (parent == class_parent_.end() || !parent->second) break;
            current = *parent->second;
        }
        return std::nullopt;
    }

    const ClassDecl* output_class(const std::string& name) const {
        const auto index = class_index_.find(name);
        if (index == class_index_.end()) return nullptr;
        return &output_.classes[index->second];
    }

    const FieldDecl* output_field(const std::string& class_name, const std::string& field) const {
        std::string current = class_name;
        std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            if (const auto* declaration = output_class(current)) {
                const auto it = std::find_if(
                    declaration->fields.begin(), declaration->fields.end(),
                    [&](const auto& candidate) { return candidate.name == field; });
                if (it != declaration->fields.end()) return &*it;
            }
            const auto parent = class_parent_.find(current);
            if (parent == class_parent_.end() || !parent->second) break;
            current = *parent->second;
        }
        return nullptr;
    }

    const FunctionDecl* output_method(const std::string& class_name, const std::string& method) const {
        std::string current = class_name;
        std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            if (const auto* declaration = output_class(current)) {
                const auto it = std::find_if(
                    declaration->methods.begin(), declaration->methods.end(),
                    [&](const auto& candidate) { return candidate.name == method; });
                if (it != declaration->methods.end()) return &*it;
            }
            const auto parent = class_parent_.find(current);
            if (parent == class_parent_.end() || !parent->second) break;
            current = *parent->second;
        }
        return nullptr;
    }

    std::optional<TypeName> method_return_type(
        const std::string& class_name,
        const std::string& method) const {
        std::string current = class_name;
        std::unordered_set<std::string> seen;
        while (!current.empty() && seen.insert(current).second) {
            if (const auto methods = method_return_types_.find(current);
                methods != method_return_types_.end()) {
                if (const auto result = methods->second.find(method);
                    result != methods->second.end()) {
                    return clone_type(result->second);
                }
            }
            const auto parent = class_parent_.find(current);
            if (parent == class_parent_.end() || !parent->second) break;
            current = *parent->second;
        }
        return std::nullopt;
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
            if (name->name == "super") {
                const auto parent = class_parent_.find(current_class);
                if (parent != class_parent_.end() && parent->second) {
                    TypeName result;
                    result.name = *parent->second;
                    result.span = expression.span;
                    return result;
                }
                return std::nullopt;
            }
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
            if (call->callee == "$std.neural.track" && call->args.size() == 1) {
                const auto source = infer_expression_type(*call->args[0].value, current_class);
                if (source && source->name == "tensor" && source->arguments.size() == 1) {
                    TypeName result;
                    result.name = "neural";
                    result.arguments.push_back(clone_type(source->arguments.front()));
                    result.tensor_rank = source->tensor_rank;
                    result.tensor_shape_prefix = source->tensor_shape_prefix;
                    result.tensor_known_shape_prefix = source->tensor_known_shape_prefix;
                    result.span = expression.span;
                    return result;
                }
            }
            if ((call->callee == "$std.neural.absolute" ||
                 call->callee == "$std.neural.exponential" ||
                 call->callee == "$std.neural.logarithm" ||
                 call->callee == "$std.neural.mean" ||
                 call->callee == "$std.neural.sum_last" ||
                 call->callee == "$std.neural.max_last" ||
                 call->callee == "$std.neural.affine" ||
                 call->callee == "$std.neural.convolve2d" ||
                 call->callee == "$std.neural.normalize" ||
                 call->callee == "$std.neural.random_mask") &&
                !call->args.empty()) {
                const auto source = infer_expression_type(*call->args[0].value, current_class);
                if (source && source->name == "neural") return source;
            }
            if (call->callee == "$std.neural.grad") {
                return simple_type("$std.neural.Gradients");
            }
            static const std::unordered_map<std::string, std::string>
                numeric_cast_result_types{
                    {"int8", "int8"}, {"int16", "int16"}, {"int32", "int32"},
                    {"int", "int"}, {"int64", "int"},
                    {"uint8", "uint8"}, {"uint16", "uint16"},
                    {"uint32", "uint32"}, {"uint64", "uint64"},
                    {"float32", "float32"}, {"float", "float"}, {"float64", "float"}};
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
                if (call->method == "contiguous" && call->args.empty()) return receiver;
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
                if (call->method == "item" && call->args.empty() &&
                    receiver->arguments.size() == 1) {
                    return clone_type(receiver->arguments.front());
                }
            }
            if (receiver->name == "neural" && call->method == "untrack" &&
                receiver->arguments.size() == 1) {
                TypeName result;
                result.name = "tensor";
                result.arguments.push_back(clone_type(receiver->arguments.front()));
                result.tensor_rank = receiver->tensor_rank;
                result.tensor_shape_prefix = receiver->tensor_shape_prefix;
                result.tensor_known_shape_prefix = receiver->tensor_known_shape_prefix;
                result.span = expression.span;
                return result;
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
        if ((pattern.name == "tensor" || pattern.name == "neural") &&
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
                continue;
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
            if (type.name == "neural") {
                if (type.arguments.size() != 1) {
                    frontend_error("GENERIC_ARITY", "neural accepts zero or one element type.", type.span);
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
            if (type.name == "$std.neural.Parameter") {
                TypeName float32 = standard_type("float32");
                const auto dimensions = type.dimensions;
                const auto span = type.span;
                const auto concrete = instantiate_class(type.name, {float32});
                TypeName result;
                result.name = concrete;
                result.dimensions = dimensions;
                result.array_depth = dimensions.size();
                result.span = span;
                return result;
            }
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
        auto out = std::make_unique<Expr>();
        out->span = source.span;

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
                    copy.callee == "$std.tensor.ones") {
                    copy.type_arguments = std::move(type_arguments);
                } else if (class_templates_.contains(copy.callee)) {
                    copy.callee = instantiate_class(copy.callee, type_arguments);
                } else if (current_class.size() && class_has_generic_method(current_class, copy.callee)) {
                    request_method_for_class(current_class, copy.callee, type_arguments, source.span);
                    copy.callee = method_name(copy.callee, type_arguments);
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
                if (const auto function = function_templates_.find(copy.callee);
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
                    if (copy.callee == "$std.neural.Parameter") {
                        TypeName float32 = standard_type("float32");
                        copy.callee = instantiate_class(copy.callee, {float32});
                    } else {
                        frontend_error(
                            "GENERIC_ARGUMENTS_REQUIRED",
                            "Generic class '" + copy.callee +
                                "' requires explicit type arguments.",
                            source.span);
                    }
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
                request_method_for_class(receiver_type->name, copy.method, type_arguments, source.span);
                copy.method = method_name(copy.method, type_arguments);
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
                case_copy.tag = case_copy.type.name;
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
            out.is_override = source.is_override;
            out.type_parameters = source.type_parameters;
            out.external_symbol = source.external_symbol;

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

        const bool neural_float_class = name == "$std.neural.Parameter";
        if (neural_float_class && !arguments.empty()) {
            const auto& element = arguments.front();
            const bool supported =
                element.arguments.empty() && element.dimensions.empty() &&
                (element.name == "float32" ||
                 element.name == "float" ||
                 element.name == "float64");
            if (!supported) {
                frontend_error(
                    "INVALID_TYPE",
                    "neural.Parameter supports only float32 or float element types.",
                    source_arguments.front().span);
            }
        }

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
        if (!building_classes_.insert(concrete_name).second) {
            frontend_error("INHERITANCE_CYCLE",
                           "Class inheritance cannot contain a cycle.",
                           source.span);
        }
        struct BuildGuard {
            std::unordered_set<std::string>& building;
            std::string name;
            ~BuildGuard() { building.erase(name); }
        } guard{building_classes_, concrete_name};

        ClassDecl out;
        out.name = concrete_name;
        out.source_file = source.source_file;
        out.span = source.span;

        if (source.parent_type) {
            out.parent_type = materialize_type(*source.parent_type, class_substitution, {});
            out.parent = out.parent_type->name;
        } else if (source.parent) {
            TypeName parent;
            parent.name = *source.parent;
            parent.span = source.span;
            out.parent_type = materialize_type(parent, class_substitution, {});
            out.parent = out.parent_type->name;
        }

        if (out.parent && concrete_classes_.contains(*out.parent) &&
            !class_index_.contains(*out.parent)) {
            materialize_concrete_class(*concrete_classes_.at(*out.parent));
        }
        class_parent_[concrete_name] = out.parent;

        for (const auto& field : source.fields) {
            FieldDecl copy;
            copy.name = field.name;
            copy.type = materialize_type(field.type, class_substitution, {});
            copy.span = field.span;
            copy.is_const = field.is_const;
            if (field.default_value) {
                copy.default_value = clone_expr(*field.default_value, class_substitution, {}, concrete_name);
            }
            out.fields.push_back(std::move(copy));
        }

        output_.classes.push_back(std::move(out));
        class_index_[concrete_name] = output_.classes.size() - 1;

        for (const auto& method : source.methods) {
            if (method.type_parameters.empty()) {
                method_return_types_[concrete_name][method.name] =
                    materialize_type(method.return_type, class_substitution, {});
            }
        }

        for (const auto& method : source.methods) {
            if (method.type_parameters.empty()) continue;
            const auto deferred = set_of(method.type_parameters);
            auto method_template = clone_function(method, class_substitution, deferred, concrete_name);
            generic_methods_[concrete_name][method.name] = std::move(method_template);
        }

        for (const auto& method : source.methods) {
            if (!method.type_parameters.empty()) continue;
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

    std::string instantiate_function(const std::string& name, const std::vector<TypeName>& source_arguments) {
        const auto templ = function_templates_.find(name);
        if (templ == function_templates_.end()) {
            frontend_error("UNKNOWN_GENERIC", "Unknown generic function '" + name + "'.");
        }

        std::vector<TypeName> arguments;
        for (const auto& argument : source_arguments) arguments.push_back(materialize_type(argument, {}, {}));

        if (arguments.size() != templ->second->type_parameters.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic function '" + name + "' expects " +
                               std::to_string(templ->second->type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           templ->second->span);
        }

        const auto key = instance_key(name, arguments);
        if (const auto existing = function_instances_.find(key); existing != function_instances_.end()) {
            return existing->second;
        }

        const auto concrete_name = mangle("gf", name, key);
        function_instances_[key] = concrete_name;

        Substitution substitution;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            substitution[templ->second->type_parameters[i]] = clone_type(arguments[i]);
        }

        function_return_types_[concrete_name] =
            materialize_type(templ->second->return_type, substitution, {});
        auto concrete = clone_function(*templ->second, substitution, {}, "");
        concrete.name = concrete_name;
        concrete.type_parameters.clear();
        output_.functions.push_back(std::move(concrete));
        return concrete_name;
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
        if (templ.is_override) {
            const auto parent = class_parent_.find(class_name);
            if (parent != class_parent_.end() && parent->second) {
                if (const auto parent_owner = generic_method_owner(*parent->second, method)) {
                    instantiate_method_for_class(*parent_owner, method, arguments);
                }
            }
        }
        if (arguments.size() != templ.type_parameters.size()) {
            frontend_error("GENERIC_ARITY",
                           "Generic method '" + method + "' expects " +
                               std::to_string(templ.type_parameters.size()) +
                               " type argument(s), got " + std::to_string(arguments.size()) + ".",
                           templ.span);
        }

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
        output_.classes[class_index_.at(class_name)].methods.push_back(std::move(concrete));
    }
};

} // namespace

ResolvedProgram load_program_with_modules(
    const std::filesystem::path& root_file,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors) {
    return ResolvedProgram{ModuleLoader(command_working_directory, max_errors).load(root_file)};
}

ResolvedProgram load_program_with_root_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors,
    bool enforce_package_lock) {
    return ResolvedProgram{
        ModuleLoader(
            command_working_directory, max_errors, std::string(root_source),
            nullptr, enforce_package_lock)
            .load(root_file)};
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

ConcreteProgram expand_generics(ResolvedProgram program) {
    return ConcreteProgram{GenericExpander(std::move(program.program)).run()};
}

} // namespace quidra
