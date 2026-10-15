#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "quidra/project.hpp"
#include "quidra/standard_classes.hpp"

namespace quidra {

struct BuiltinTypeName {
    std::string_view name;
    std::string_view canonical;
};

// The spellings of the built-in types and the canonical name each one
// denotes.
inline constexpr std::array<BuiltinTypeName, 27> builtin_type_names{{
    {"int", "int"},
    {"nat", "nat"},
    {"int8", "int8"},
    {"int16", "int16"},
    {"int32", "int32"},
    {"int64", "int64"},
    {"nat8", "nat8"},
    {"nat16", "nat16"},
    {"nat32", "nat32"},
    {"nat64", "nat64"},
    {"real16", "real16"},
    {"real16b", "real16b"},
    {"real32", "real32"},
    {"real64", "real64"},
    {"real", "real"},
    {"com16", "com16"},
    {"com16b", "com16b"},
    {"com32", "com32"},
    {"com64", "com64"},
    {"com", "com"},
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
           name == "func" || name == "auto" || name == "union" ||
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
    Array,
    Len,
    ExactAtom,
    ExactUnary,
    ExactOp,
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
    ReflectEachPaths,
    ReflectEach,
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
inline constexpr std::array<BuiltinCallableInfo, 6> builtin_callables{{
    {"print", BuiltinCallable::Print},
    {"flush", BuiltinCallable::Flush},
    {"scan", BuiltinCallable::Scan},
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
    {"$std.autograd.target", BuiltinCallable::AutogradTarget},
    {"$std.random.generator", BuiltinCallable::RandomGenerator},
    {"$std.random.int", BuiltinCallable::RandomInt},
    {"$std.random.real64", BuiltinCallable::RandomFloat},
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
    {"$std.json.real", BuiltinCallable::JsonBigReal},
    {"$std.json.boolean", BuiltinCallable::JsonBoolean},
    {"$std.json.encode", BuiltinCallable::JsonEncode},
    {"$std.json.equal", BuiltinCallable::JsonEqual},
    {"$std.http.get", BuiltinCallable::HttpGet},
    {"$std.http.header", BuiltinCallable::HttpHeader},
    {"$std.tensor.zeros", BuiltinCallable::TensorZeros},
    {"$std.tensor.ones", BuiltinCallable::TensorOnes},
    {"$std.real.atom", BuiltinCallable::ExactAtom},
    {"$std.real.unary", BuiltinCallable::ExactUnary},
    {"$std.real.op", BuiltinCallable::ExactOp},
    {"$std.reflect.collect", BuiltinCallable::ReflectCollect},
    {"$std.reflect.paths", BuiltinCallable::ReflectPaths},
    {"$std.reflect.each_paths", BuiltinCallable::ReflectEachPaths},
    {"$std.reflect.each", BuiltinCallable::ReflectEach},
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

// ---- Effects of the built-ins (L1, L2) ------------------------------------
// What the built-in callables and the methods of built-in types change or
// use besides their `&` arguments. The effect summaries read these tables,
// and a method that changes existing state needs the mutation marker at
// every call (`.handle.write(text)`). External I/O is never such a change;
// it only takes part in conflicts. The methods with a Quidra body
// (map.Map, set.Set, random.Generator) declare their marker in that body.

// The external resources, in the order of semantics::ResourceKind.
enum class BuiltinResource : unsigned char {
    Stdout,
    Stdin,
    Stderr,
    FileSystem,
    Process,
    Network,
    Clock,
    Environment,
};
// Shared commutes with other shared uses of the resource; Exclusive does not.
enum class BuiltinResourceUse : unsigned char { Shared, Exclusive };

struct BuiltinResourceAccess {
    BuiltinResource resource;
    BuiltinResourceUse use;
};

struct BuiltinCallableEffect {
    BuiltinCallable kind;
    // At most four resource uses; `count` of them are valid.
    std::array<BuiltinResourceAccess, 4> resources;
    unsigned char count;
    // A generator method's built-in: it advances the receiver's state.
    bool advances_receiver;
};

inline constexpr std::array<BuiltinCallableEffect, 28> builtin_callable_effects{{
    {BuiltinCallable::Print, {{{BuiltinResource::Stdout, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::Flush, {{{BuiltinResource::Stdout, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::Scan, {{{BuiltinResource::Stdin, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileRead, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileReadBin, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileExists, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileIsDirectory, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileList, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileOpen, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::FileWrite, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileWriteBin, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileRemove, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileCopy, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileMove, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileMkdir, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileCreate, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::FileAppend, {{{BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::ProcessRun,
     {{{BuiltinResource::Process, BuiltinResourceUse::Exclusive},
       {BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive},
       {BuiltinResource::Network, BuiltinResourceUse::Exclusive},
       {BuiltinResource::Stdin, BuiltinResourceUse::Exclusive}}},
     4, false},
    {BuiltinCallable::ProcessShell,
     {{{BuiltinResource::Process, BuiltinResourceUse::Exclusive},
       {BuiltinResource::FileSystem, BuiltinResourceUse::Exclusive},
       {BuiltinResource::Network, BuiltinResourceUse::Exclusive},
       {BuiltinResource::Stdin, BuiltinResourceUse::Exclusive}}},
     4, false},
    {BuiltinCallable::HttpGet, {{{BuiltinResource::Network, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::TimeNow, {{{BuiltinResource::Clock, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::TimeSince, {{{BuiltinResource::Clock, BuiltinResourceUse::Exclusive}}}, 1, false},
    {BuiltinCallable::TimeSleep, {{{BuiltinResource::Clock, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::EnvironmentGet, {{{BuiltinResource::Environment, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::EnvironmentHas, {{{BuiltinResource::Environment, BuiltinResourceUse::Shared}}}, 1, false},
    {BuiltinCallable::RandomInt, {}, 0, true},
    {BuiltinCallable::RandomFloat, {}, 0, true},
    {BuiltinCallable::RandomBool, {}, 0, true},
}};

// The effects of `kind`, or null when it changes nothing and uses no
// resource.
inline constexpr const BuiltinCallableEffect* builtin_callable_effect(BuiltinCallable kind) {
    for (const auto& effect : builtin_callable_effects) {
        if (effect.kind == kind) return &effect;
    }
    return nullptr;
}

// What a method of a built-in type changes besides its arguments.
enum class StandardMethodChange : unsigned char {
    // The receiver's own value (a tensor or Target gradient).
    Receiver,
    // The shared state the receiver holds, which every copy of the receiver
    // shares (a file position or close state).
    SharedState,
};
// The file access of a file.Handle method, through that handle.
enum class StandardMethodFileAccess : unsigned char { None, Read, Write };

struct StandardMethodEffect {
    // "tensor", or the standard class id (quidra/standard_classes.hpp).
    std::string_view receiver;
    std::string_view method;
    StandardMethodChange change;
    StandardMethodFileAccess file;
};

// Every method of a built-in type that changes existing state: each needs
// the mutation marker. tensor.track(), retrack() and untrack() return a new
// value and leave their receiver as it was, so they are not listed.
inline constexpr std::array<StandardMethodEffect, 11> standard_method_effects{{
    {"tensor", "clear_grad", StandardMethodChange::Receiver, StandardMethodFileAccess::None},
    {"$std.file.Handle", "read", StandardMethodChange::SharedState, StandardMethodFileAccess::Read},
    {"$std.file.Handle", "read_line", StandardMethodChange::SharedState, StandardMethodFileAccess::Read},
    {"$std.file.Handle", "read_bin", StandardMethodChange::SharedState, StandardMethodFileAccess::Read},
    {"$std.file.Handle", "seek", StandardMethodChange::SharedState, StandardMethodFileAccess::Read},
    {"$std.file.Handle", "write", StandardMethodChange::SharedState, StandardMethodFileAccess::Write},
    {"$std.file.Handle", "write_line", StandardMethodChange::SharedState, StandardMethodFileAccess::Write},
    {"$std.file.Handle", "flush", StandardMethodChange::SharedState, StandardMethodFileAccess::Write},
    {"$std.file.Handle", "close", StandardMethodChange::SharedState, StandardMethodFileAccess::Write},
    {"$std.autograd.Target", "clear_grad", StandardMethodChange::Receiver, StandardMethodFileAccess::None},
}};

// The effect of the method `method` of a value whose type is `receiver`
// ("tensor" or a standard class id), or null when the method changes
// nothing besides its arguments.
inline constexpr const StandardMethodEffect* standard_method_effect(std::string_view receiver,
                                                                   std::string_view method) {
    for (const auto& effect : standard_method_effects) {
        if (effect.receiver == receiver && effect.method == method) return &effect;
    }
    return nullptr;
}

inline constexpr std::array<std::string_view, 18> standard_modules{{
    "cli", "file", "environment", "test", "time", "gpu", "autograd", "reflect", "random", "process",
    "map", "set", "json", "http", "tensor"
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


    if (module == "autograd") {
        if (member == "target") return "$std.autograd.target";
        return std::nullopt;
    }
    if (module == "reflect") {
        if (member == "collect") return "$std.reflect.collect";
        if (member == "paths") return "$std.reflect.paths";
        if (member == "each") return "$std.reflect.each";
        if (member == "each_paths") return "$std.reflect.each_paths";
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

// The functions a built-in type name provides as a namespace:
// real.atom(provider, opcode), real.unary(provider, opcode, value) and
// real.op(provider, opcode, values...), the exact-real extension intrinsics.
inline constexpr std::optional<std::string_view> standard_type_function_target(
    std::string_view type, std::string_view member) {
    if (type == "real") {
        if (member == "atom") return "$std.real.atom";
        if (member == "unary") return "$std.real.unary";
        if (member == "op") return "$std.real.op";
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

// Ordinary class bindings need an explicitly supplied instance. Only standard
// map/set bindings establish their compiler-owned empty storage at declaration.
inline constexpr bool class_storage_established_at_declaration(
    std::string_view class_name, bool standard_library) {
    return standard_library &&
        (class_name.starts_with(standard_class::map_instance_prefix) ||
         class_name.starts_with(standard_class::set_instance_prefix));
}

// `_` is the discard name: where a construct binds a value it does not use,
// `_` binds nothing. It therefore never names a declaration and can never be
// read. The frontend and the checker report both with DISCARD.
inline constexpr std::string_view discard_name = "_";
inline constexpr bool is_discard_name(std::string_view name) { return name == discard_name; }
inline constexpr char discard_declaration_message[] =
    "'_' is the discard name; it cannot name a variable, parameter, field, function, "
    "class, enum, module or type.";
inline constexpr char discard_typed_message[] = "'_' takes no type; its type is inferred.";
inline constexpr char discard_read_message[] = "'_' discards a value; it cannot be read.";

// Names that start with "__quidra_" (abi::symbol_namespace::internal_prefix)
// belong to the compiler: it gives them to the instances of generic
// declarations and to its own helpers. No source declares one (SHADOWING).
inline std::string compiler_name_message(std::string_view name) {
    return "'" + std::string(name) +
           "' starts with '__quidra_', which is reserved for the names the compiler makes.";
}

inline constexpr bool is_reserved_value_name(std::string_view name) {
    return is_builtin_text_constant(name) || is_builtin_callable(name) ||
           is_builtin_type_name(name) || is_standard_module(name) ||
           name == "func" || name == "fn" || name == "auto" || name == "union" ||
           name == "main" || name == "parallel" || name == "local" || name == "extend";
}

inline std::string builtin_types_json() {
    std::string out = "[";
    bool first = true;
    for (const auto& type : builtin_type_names) {
        if (!first) out += ",";
        first = false;
        out += "\"" + std::string(type.name) + "\"";
    }
    out += ",\"func<R>(A, B)\",\"T[]\",\"T[n]\",\"T | U\",\"class Name\"]";
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
