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
    return is_builtin_type_name(name) || name == "tensor" || name == "neural" ||
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
    Write,
    Scan,
    IoFlush,
    Exit,
    Range,
    Array,
    Len,
    Abs,
    Sqrt,
    Min,
    Max,
    MathSin,
    MathCos,
    MathTan,
    MathLog,
    MathExp,
    MathPow,
    MathTrunc,
    MathRound,
    MathFloor,
    MathCeil,
    MathIsFinite,
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
    VideoOpen,
    VideoRead,
    VideoWidth,
    VideoHeight,
    VideoFps,
    TensorCreate,
    TensorZeros,
    TensorOnes,
    StatsSum,
    StatsMean,
    StatsMin,
    StatsMax,
    LinearMatmul,
    LinearDot,
    ImageRead,
    ImageWrite,
    ImageTensorCrop,
    ImageTensorResize,
    ImageTensorFlipHorizontal,
    ImageTensorFlipVertical,
    ImageTensorRotate90,
    ImageTensorRotate180,
    ImageTensorRotate270,
    ImageTensorGrayscale,
    ImageTensorThreshold,
    ImageTensorBlur,
    ImageTensorFilter,
    ImageTensorDilate,
    ImageTensorErode,
    NeuralTrack,
    NeuralParameterTrack,
    NeuralAffine,
    NeuralConvolve2D,
    NeuralAbsolute,
    NeuralExponential,
    NeuralLogarithm,
    NeuralMean,
    NeuralSumLast,
    NeuralMaxLast,
    NeuralUpdate,
    NeuralNormalize,
    NeuralNormalizeInference,
    NeuralRandomMask,
    NeuralMomentUpdate,
    NeuralGrad,
    NeuralAllReduceSum,
    NeuralSave,
    NeuralLoad
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
    {"write", BuiltinCallable::Write},
    {"scan", BuiltinCallable::Scan},
    {"range", BuiltinCallable::Range},
    {"array", BuiltinCallable::Array},
    {"len", BuiltinCallable::Len},
    {"tensor", BuiltinCallable::TensorCreate},
}};

inline constexpr std::array<BuiltinCallableInfo, 115> intrinsic_callables{{
    {"$std.math.abs", BuiltinCallable::Abs},
    {"$std.math.sqrt", BuiltinCallable::Sqrt},
    {"$std.math.min", BuiltinCallable::Min},
    {"$std.math.max", BuiltinCallable::Max},
    {"$std.math.sin", BuiltinCallable::MathSin},
    {"$std.math.cos", BuiltinCallable::MathCos},
    {"$std.math.tan", BuiltinCallable::MathTan},
    {"$std.math.log", BuiltinCallable::MathLog},
    {"$std.math.exp", BuiltinCallable::MathExp},
    {"$std.math.pow", BuiltinCallable::MathPow},
    {"$std.math.trunc", BuiltinCallable::MathTrunc},
    {"$std.math.round", BuiltinCallable::MathRound},
    {"$std.math.floor", BuiltinCallable::MathFloor},
    {"$std.math.ceil", BuiltinCallable::MathCeil},
    {"$std.math.is_finite", BuiltinCallable::MathIsFinite},
    {"$std.io.flush", BuiltinCallable::IoFlush},
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
    {"$std.video.open", BuiltinCallable::VideoOpen},
    {"$std.video.read", BuiltinCallable::VideoRead},
    {"$std.video.width", BuiltinCallable::VideoWidth},
    {"$std.video.height", BuiltinCallable::VideoHeight},
    {"$std.video.fps", BuiltinCallable::VideoFps},
    {"$std.tensor.zeros", BuiltinCallable::TensorZeros},
    {"$std.tensor.ones", BuiltinCallable::TensorOnes},
    {"$std.stats.sum", BuiltinCallable::StatsSum},
    {"$std.stats.mean", BuiltinCallable::StatsMean},
    {"$std.stats.min", BuiltinCallable::StatsMin},
    {"$std.stats.max", BuiltinCallable::StatsMax},
    {"$std.linear.matmul", BuiltinCallable::LinearMatmul},
    {"$std.linear.dot", BuiltinCallable::LinearDot},
    {"$std.image.read", BuiltinCallable::ImageRead},
    {"$std.image.write", BuiltinCallable::ImageWrite},
    {"$std.image.tensor_crop", BuiltinCallable::ImageTensorCrop},
    {"$std.image.tensor_resize", BuiltinCallable::ImageTensorResize},
    {"$std.image.tensor_flip_horizontal", BuiltinCallable::ImageTensorFlipHorizontal},
    {"$std.image.tensor_flip_vertical", BuiltinCallable::ImageTensorFlipVertical},
    {"$std.image.tensor_rotate90", BuiltinCallable::ImageTensorRotate90},
    {"$std.image.tensor_rotate180", BuiltinCallable::ImageTensorRotate180},
    {"$std.image.tensor_rotate270", BuiltinCallable::ImageTensorRotate270},
    {"$std.image.tensor_grayscale", BuiltinCallable::ImageTensorGrayscale},
    {"$std.image.tensor_threshold", BuiltinCallable::ImageTensorThreshold},
    {"$std.image.tensor_blur", BuiltinCallable::ImageTensorBlur},
    {"$std.image.tensor_filter", BuiltinCallable::ImageTensorFilter},
    {"$std.image.tensor_dilate", BuiltinCallable::ImageTensorDilate},
    {"$std.image.tensor_erode", BuiltinCallable::ImageTensorErode},
    {"$std.neural.track", BuiltinCallable::NeuralTrack},
    {"$std.neural.parameter_track", BuiltinCallable::NeuralParameterTrack},
    {"$std.neural.affine", BuiltinCallable::NeuralAffine},
    {"$std.neural.convolve2d", BuiltinCallable::NeuralConvolve2D},
    {"$std.neural.absolute", BuiltinCallable::NeuralAbsolute},
    {"$std.neural.exponential", BuiltinCallable::NeuralExponential},
    {"$std.neural.logarithm", BuiltinCallable::NeuralLogarithm},
    {"$std.neural.mean", BuiltinCallable::NeuralMean},
    {"$std.neural.sum_last", BuiltinCallable::NeuralSumLast},
    {"$std.neural.max_last", BuiltinCallable::NeuralMaxLast},
    {"$std.neural.update", BuiltinCallable::NeuralUpdate},
    {"$std.neural.normalize", BuiltinCallable::NeuralNormalize},
    {"$std.neural.normalize_inference", BuiltinCallable::NeuralNormalizeInference},
    {"$std.neural.random_mask", BuiltinCallable::NeuralRandomMask},
    {"$std.neural.moment_update", BuiltinCallable::NeuralMomentUpdate},
    {"$std.neural.grad", BuiltinCallable::NeuralGrad},
    {"$std.neural.all_reduce_sum", BuiltinCallable::NeuralAllReduceSum},
    {"$std.neural.save", BuiltinCallable::NeuralSave},
    {"$std.neural.load", BuiltinCallable::NeuralLoad},
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

inline constexpr std::array<std::string_view, 24> standard_modules{{
    "math", "io", "cli", "file", "environment", "test", "time", "gpu", "task", "atomic", "ref", "random", "process",
    "map", "set", "json", "http", "stats", "linear", "signal", "image", "video", "tensor",
    "neural"
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
    if (module == "io") {
        if (member == "flush") return "$std.io.flush";
        return std::nullopt;
    }
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
    if (module == "video") {
        if (member == "open") return "$std.video.open";
        return std::nullopt;
    }
    if (module == "tensor") {
        if (member == "zeros") return "$std.tensor.zeros";
        if (member == "ones") return "$std.tensor.ones";
        return std::nullopt;
    }
    if (module == "stats") {
        if (member == "sum") return "$std.stats.sum";
        if (member == "mean") return "$std.stats.mean";
        if (member == "min") return "$std.stats.min";
        if (member == "max") return "$std.stats.max";
        return std::nullopt;
    }
    if (module == "linear") {
        if (member == "matmul") return "$std.linear.matmul";
        if (member == "dot") return "$std.linear.dot";
        return std::nullopt;
    }
    if (module == "image") {
        if (member == "read") return "$std.image.read";
        if (member == "write") return "$std.image.write";
        if (member == "tensor_crop") return "$std.image.tensor_crop";
        if (member == "tensor_resize") return "$std.image.tensor_resize";
        if (member == "tensor_flip_horizontal") return "$std.image.tensor_flip_horizontal";
        if (member == "tensor_flip_vertical") return "$std.image.tensor_flip_vertical";
        if (member == "tensor_rotate90") return "$std.image.tensor_rotate90";
        if (member == "tensor_rotate180") return "$std.image.tensor_rotate180";
        if (member == "tensor_rotate270") return "$std.image.tensor_rotate270";
        if (member == "tensor_grayscale") return "$std.image.tensor_grayscale";
        if (member == "tensor_threshold") return "$std.image.tensor_threshold";
        if (member == "tensor_blur") return "$std.image.tensor_blur";
        if (member == "tensor_filter") return "$std.image.tensor_filter";
        if (member == "tensor_dilate") return "$std.image.tensor_dilate";
        if (member == "tensor_erode") return "$std.image.tensor_erode";
        return std::nullopt;
    }
    if (module == "neural") {
        if (member == "track") return "$std.neural.track";
        if (member == "affine") return "$std.neural.affine";
        if (member == "convolve2d") return "$std.neural.convolve2d";
        if (member == "absolute") return "$std.neural.absolute";
        if (member == "exponential") return "$std.neural.exponential";
        if (member == "logarithm") return "$std.neural.logarithm";
        if (member == "mean") return "$std.neural.mean";
        if (member == "sum_last") return "$std.neural.sum_last";
        if (member == "max_last") return "$std.neural.max_last";
        if (member == "update") return "$std.neural.update";
        if (member == "normalize") return "$std.neural.normalize";
        if (member == "normalize_inference") return "$std.neural.normalize_inference";
        if (member == "random_mask") return "$std.neural.random_mask";
        if (member == "moment_update") return "$std.neural.moment_update";
        if (member == "grad") return "$std.neural.grad";
        if (member == "all_reduce_sum") return "$std.neural.all_reduce_sum";
        if (member == "save") return "$std.neural.save";
        if (member == "load") return "$std.neural.load";
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
    if (module != "math") return std::nullopt;
    if (member == "abs") return "$std.math.abs";
    if (member == "sqrt") return "$std.math.sqrt";
    if (member == "min") return "$std.math.min";
    if (member == "max") return "$std.math.max";
    if (member == "sin") return "$std.math.sin";
    if (member == "cos") return "$std.math.cos";
    if (member == "tan") return "$std.math.tan";
    if (member == "log") return "$std.math.log";
    if (member == "exp") return "$std.math.exp";
    if (member == "pow") return "$std.math.pow";
    if (member == "trunc") return "$std.math.trunc";
    if (member == "round") return "$std.math.round";
    if (member == "floor") return "$std.math.floor";
    if (member == "ceil") return "$std.math.ceil";
    if (member == "is_finite") return "$std.math.is_finite";
    return std::nullopt;
}

inline constexpr std::optional<std::string_view> standard_value_target(
    std::string_view module, std::string_view member) {
    if (module != "math") return std::nullopt;
    if (member == "pi") return "$std.math.pi";
    if (member == "e") return "$std.math.e";
    return std::nullopt;
}

inline constexpr bool is_standard_real_constant(std::string_view name) {
    return name == "$std.math.pi" || name == "$std.math.e";
}

inline constexpr std::optional<double> standard_float_constant(std::string_view name) {
    if (name == "$std.math.pi") return 3.141592653589793238462643383279502884;
    if (name == "$std.math.e") return 2.718281828459045235360287471352662498;
    return std::nullopt;
}

inline constexpr bool is_builtin_callable(std::string_view name) {
    return builtin_callable(name).has_value();
}

inline constexpr std::array<std::pair<std::string_view, std::string_view>, 8> builtin_text_constants{{
    {"ENTER", "\n"},
    {"TAB", "\t"},
    {"HOME", "\r"},
    {"QUOTE", "\""},
    {"BACKSPACE", "\b"},
    {"PAGE", "\f"},
    {"VTAB", "\v"},
    {"BELL", "\a"},
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
// them as language-provided symbols rather than user bindings. Before language
// 0.2 they were lowercase; a lowercase spelling now resolves to nothing, and
// this names the constant the writer most likely meant.
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
           name == "construct";
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
