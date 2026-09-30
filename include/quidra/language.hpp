#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "quidra/project.hpp"

namespace quidra {

struct BuiltinTypeName {
    std::string_view name;
    std::string_view canonical;
};

inline constexpr std::array<BuiltinTypeName, 20> builtin_type_names{{
    {"int", "int"},
    {"int8", "int8"},
    {"int16", "int16"},
    {"int32", "int32"},
    {"int64", "int"},
    {"uint8", "uint8"},
    {"uint16", "uint16"},
    {"uint32", "uint32"},
    {"uint64", "uint64"},
    {"bigint", "bigint"},
    {"float", "float"},
    {"float32", "float32"},
    {"float64", "float"},
    {"bigreal", "bigreal"},
    {"bool", "bool"},
    {"string", "string"},
    {"bin", "bin"},
    {"void", "void"},
    {"none", "none"},
    {"error", "error"},
}};

inline constexpr bool is_builtin_type_name(std::string_view name) {
    for (const auto& type : builtin_type_names) {
        if (type.name == name) return true;
    }
    return false;
}

inline constexpr bool is_language_type_name(std::string_view name) {
    return is_builtin_type_name(name) || name == "tensor" ||
           name == "fn" || name == "auto" || name == "union" ||
           name == "extern";
}

inline constexpr std::optional<std::string_view> canonical_builtin_type_name(std::string_view name) {
    for (const auto& type : builtin_type_names) {
        if (type.name == name) return type.canonical;
    }
    return std::nullopt;
}

enum class BuiltinCallable {
    Print,
    Scan,
    Flush,
    Exit,
    Range,
    Array,
    Len,
    ExactAtom,
    ExactUnary,
    CliArgument,
    CliArgumentOptional,
    CliOption,
    CliFlag,
    CliFinish,
    FileOpen,
    FileCreate,
    FileAppend,
    FileRead,
    FileReadBin,
    FileWrite,
    FileWriteBin,
    FileExists,
    FileIsDirectory,
    FileRemove,
    FileCopy,
    FileMove,
    FileMkdir,
    FileList,
    EnvironmentGet,
    EnvironmentHas,
    TestCheck,
    TestEqual,
    TimeNow,
    TimeSince,
    TimeSeconds,
    TimeSleep,
    GpuSync,
    TaskAll,
    AtomicCounter,
    AutogradTarget,
    RandomGenerator,
    RandomInt,
    RandomFloat,
    RandomBool,
    ProcessRun,
    ProcessShell,
    JsonParse,
    JsonKind,
    JsonSize,
    JsonGet,
    JsonAt,
    JsonText,
    JsonInteger,
    JsonNumber,
    JsonBigInt,
    JsonBigReal,
    JsonBoolean,
    JsonEncode,
    JsonEqual,
    HttpGet,
    HttpHeader,
    TensorCreate,
    TensorZeros,
    TensorOnes,
    ReflectCollect,
    ReflectPaths,
    ReflectTypeName,
};

struct BuiltinCallableInfo {
    std::string_view name;
    BuiltinCallable kind;
};

// Bare built-ins are part of the language-visible namespace. Under Quidra's
// monotonic name-resolution rule, adding an entry here reserves a previously
// usable source name and is therefore a compatibility-breaking language change.
// Prefer namespaced standard-library functions or value methods for new APIs.
inline constexpr std::array<BuiltinCallableInfo, 7> builtin_callables{{
    {"print", BuiltinCallable::Print},
    {"flush", BuiltinCallable::Flush},
    {"scan", BuiltinCallable::Scan},
    {"range", BuiltinCallable::Range},
    {"array", BuiltinCallable::Array},
    {"len", BuiltinCallable::Len},
    {"tensor", BuiltinCallable::TensorCreate},
}};

inline constexpr std::array<BuiltinCallableInfo, 60> intrinsic_callables{{
    {"$std.cli.argument", BuiltinCallable::CliArgument},
    {"$std.cli.argument_optional", BuiltinCallable::CliArgumentOptional},
    {"$std.cli.option", BuiltinCallable::CliOption},
    {"$std.cli.flag", BuiltinCallable::CliFlag},
    {"$std.cli.finish", BuiltinCallable::CliFinish},
    {"$std.file.open", BuiltinCallable::FileOpen},
    {"$std.file.create", BuiltinCallable::FileCreate},
    {"$std.file.append", BuiltinCallable::FileAppend},
    {"$std.file.read", BuiltinCallable::FileRead},
    {"$std.file.read_bin", BuiltinCallable::FileReadBin},
    {"$std.file.write", BuiltinCallable::FileWrite},
    {"$std.file.write_bin", BuiltinCallable::FileWriteBin},
    {"$std.file.exists", BuiltinCallable::FileExists},
    {"$std.file.is_directory", BuiltinCallable::FileIsDirectory},
    {"$std.file.remove", BuiltinCallable::FileRemove},
    {"$std.file.copy", BuiltinCallable::FileCopy},
    {"$std.file.move", BuiltinCallable::FileMove},
    {"$std.file.mkdir", BuiltinCallable::FileMkdir},
    {"$std.file.list", BuiltinCallable::FileList},
    {"$std.environment.get", BuiltinCallable::EnvironmentGet},
    {"$std.environment.has", BuiltinCallable::EnvironmentHas},
    {"$std.test.check", BuiltinCallable::TestCheck},
    {"$std.test.equal", BuiltinCallable::TestEqual},
    {"$std.time.now", BuiltinCallable::TimeNow},
    {"$std.time.since", BuiltinCallable::TimeSince},
    {"$std.time.seconds", BuiltinCallable::TimeSeconds},
    {"$std.time.sleep", BuiltinCallable::TimeSleep},
    {"$std.gpu.sync", BuiltinCallable::GpuSync},
    {"$std.task.all", BuiltinCallable::TaskAll},
    {"$std.atomic.counter", BuiltinCallable::AtomicCounter},
    {"$std.autograd.target", BuiltinCallable::AutogradTarget},
    {"$std.random.generator", BuiltinCallable::RandomGenerator},
    {"$std.random.int", BuiltinCallable::RandomInt},
    {"$std.random.float", BuiltinCallable::RandomFloat},
    {"$std.random.bool", BuiltinCallable::RandomBool},
    {"$std.process.run", BuiltinCallable::ProcessRun},
    {"$std.process.shell", BuiltinCallable::ProcessShell},
    {"$std.process.exit", BuiltinCallable::Exit},
    {"$std.json.parse", BuiltinCallable::JsonParse},
    {"$std.json.kind", BuiltinCallable::JsonKind},
    {"$std.json.size", BuiltinCallable::JsonSize},
    {"$std.json.get", BuiltinCallable::JsonGet},
    {"$std.json.at", BuiltinCallable::JsonAt},
    {"$std.json.text", BuiltinCallable::JsonText},
    {"$std.json.integer", BuiltinCallable::JsonInteger},
    {"$std.json.number", BuiltinCallable::JsonNumber},
    {"$std.json.bigint", BuiltinCallable::JsonBigInt},
    {"$std.json.bigreal", BuiltinCallable::JsonBigReal},
    {"$std.json.boolean", BuiltinCallable::JsonBoolean},
    {"$std.json.encode", BuiltinCallable::JsonEncode},
    {"$std.json.equal", BuiltinCallable::JsonEqual},
    {"$std.http.get", BuiltinCallable::HttpGet},
    {"$std.http.header", BuiltinCallable::HttpHeader},
    {"$std.tensor.zeros", BuiltinCallable::TensorZeros},
    {"$std.tensor.ones", BuiltinCallable::TensorOnes},
    {"$std.exact.atom", BuiltinCallable::ExactAtom},
    {"$std.exact.unary", BuiltinCallable::ExactUnary},
    {"$std.reflect.collect", BuiltinCallable::ReflectCollect},
    {"$std.reflect.paths", BuiltinCallable::ReflectPaths},
    {"$std.reflect.type_name", BuiltinCallable::ReflectTypeName},
}};

inline constexpr std::optional<BuiltinCallable> builtin_callable(std::string_view name) {
    for (const auto& callable : builtin_callables) {
        if (callable.name == name) return callable.kind;
    }
    for (const auto& callable : intrinsic_callables) {
        if (callable.name == name) return callable.kind;
    }
    return std::nullopt;
}

inline constexpr std::array<std::string_view, 19> standard_modules{{
    "cli", "file", "environment", "test", "time", "gpu", "task", "atomic", "autograd", "ref", "reflect", "random", "process",
    "map", "set", "json", "http", "tensor", "exact"
}};

inline constexpr bool is_standard_module(std::string_view name) {
    for (const auto module : standard_modules) {
        if (module == name) return true;
    }
    return false;
}

inline std::string standard_modules_json() {
    std::string out = "[";
    for (std::size_t i = 0; i < standard_modules.size(); ++i) {
        if (i) out += ",";
        out += "\"" + std::string(standard_modules[i]) + "\"";
    }
    out += "]";
    return out;
}

inline constexpr std::optional<std::string_view> standard_function_target(
    std::string_view module, std::string_view member) {
    if (module == "environment") {
        if (member == "get") return "$std.environment.get";
        if (member == "has") return "$std.environment.has";
        return std::nullopt;
    }
    if (module == "test") {
        if (member == "check") return "$std.test.check";
        if (member == "equal") return "$std.test.equal";
        return std::nullopt;
    }
    if (module == "time") {
        if (member == "now") return "$std.time.now";
        if (member == "since") return "$std.time.since";
        if (member == "seconds") return "$std.time.seconds";
        if (member == "sleep") return "$std.time.sleep";
        return std::nullopt;
    }
    if (module == "gpu") {
        if (member == "sync") return "$std.gpu.sync";
        return std::nullopt;
    }
    if (module == "task") {
        if (member == "all") return "$std.task.all";
        return std::nullopt;
    }
    if (module == "atomic") {
        if (member == "counter") return "$std.atomic.counter";
        return std::nullopt;
    }
    if (module == "autograd") {
        if (member == "target") return "$std.autograd.target";
        return std::nullopt;
    }
    if (module == "reflect") {
        if (member == "collect") return "$std.reflect.collect";
        if (member == "paths") return "$std.reflect.paths";
        if (member == "type_name") return "$std.reflect.type_name";
        return std::nullopt;
    }
    if (module == "random") {
        if (member == "generator") return "$std.random.generator";
        return std::nullopt;
    }
    if (module == "process") {
        if (member == "run") return "$std.process.run";
        if (member == "shell") return "$std.process.shell";
        if (member == "exit") return "$std.process.exit";
        return std::nullopt;
    }
    if (module == "json") {
        if (member == "parse") return "$std.json.parse";
        return std::nullopt;
    }
    if (module == "http") {
        if (member == "get") return "$std.http.get";
        return std::nullopt;
    }
    if (module == "tensor") {
        if (member == "zeros") return "$std.tensor.zeros";
        if (member == "ones") return "$std.tensor.ones";
        return std::nullopt;
    }
    if (module == "exact") {
        if (member == "atom") return "$std.exact.atom";
        if (member == "unary") return "$std.exact.unary";
        return std::nullopt;
    }
    if (module == "file") {
        if (member == "open") return "$std.file.open";
        if (member == "create") return "$std.file.create";
        if (member == "append") return "$std.file.append";
        if (member == "read") return "$std.file.read";
        if (member == "read_bin") return "$std.file.read_bin";
        if (member == "write") return "$std.file.write";
        if (member == "write_bin") return "$std.file.write_bin";
        if (member == "exists") return "$std.file.exists";
        if (member == "is_directory") return "$std.file.is_directory";
        if (member == "remove") return "$std.file.remove";
        if (member == "copy") return "$std.file.copy";
        if (member == "move") return "$std.file.move";
        if (member == "mkdir") return "$std.file.mkdir";
        if (member == "list") return "$std.file.list";
        return std::nullopt;
    }
    return std::nullopt;
}

inline constexpr bool is_builtin_callable(std::string_view name) {
    return builtin_callable(name).has_value();
}

inline constexpr std::array<std::pair<std::string_view, std::string_view>, 8> builtin_text_constants{{
    {"NL", "\n"},
    {"HT", "\t"},
    {"CR", "\r"},
    {"DQ", "\""},
    {"BS", "\b"},
    {"FF", "\f"},
    {"VT", "\v"},
    {"BL", "\a"},
}};

inline constexpr bool is_builtin_text_constant(std::string_view name) {
    for (const auto& [candidate, value] : builtin_text_constants) {
        (void)value;
        if (candidate == name) return true;
    }
    return false;
}

inline constexpr std::string_view builtin_text_constant(std::string_view name) {
    for (const auto& [candidate, value] : builtin_text_constants) {
        if (candidate == name) return value;
    }
    return {};
}

// The text constants are the only built-in values spelled in capitals: they
// stand for characters that cannot be written directly, and the capitals mark
// them as language-provided symbols rather than user bindings. Legacy lowercase
// spellings resolve to nothing; this names the constant the writer most likely
// meant so diagnostics can point to the canonical spelling.
inline constexpr std::string_view renamed_text_constant(std::string_view name) {
    for (const auto& [candidate, value] : builtin_text_constants) {
        (void)value;
        if (candidate.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (c != candidate[i]) { same = false; break; }
        }
        if (same && candidate != name) return candidate;
    }
    return {};
}

// `Point point` creates the value at the declaration, with its declared
// defaults, so fields can be assigned one by one. Standard-library value
// types that user code cannot construct (file handles, generators, process
// results and the like) stay uninitialized until the library supplies them.
inline constexpr bool class_storage_established_at_declaration(std::string_view class_name) {
    return !class_name.starts_with("$std.");
}

inline constexpr bool is_reserved_value_name(std::string_view name) {
    return is_builtin_text_constant(name) || is_builtin_callable(name) ||
           is_builtin_type_name(name) || is_standard_module(name) ||
           name == "fn" || name == "auto" || name == "union" ||
           name == "construct" || name == "main";
}

inline std::string builtin_types_json() {
    std::string out = "[";
    bool first = true;
    for (const auto& type : builtin_type_names) {
        if (!first) out += ",";
        first = false;
        out += "\"" + std::string(type.name) + "\"";
    }
    out += ",\"fn<R>(A, B)\",\"T[]\",\"T[n]\",\"T | U\",\"class Name\"]";
    return out;
}

inline std::string builtin_values_json() {
    std::string out = "[";
    bool first = true;
    for (const auto& callable : builtin_callables) {
        if (!first) out += ",";
        first = false;
        out += "\"" + std::string(callable.name) + "\"";
    }
    out += ",\"error\"";
    for (const auto& [name, value] : builtin_text_constants) {
        (void)value;
        out += ",\"" + std::string(name) + "\"";
    }
    out += "]";
    return out;
}

} // namespace quidra