#include "toolchain/native_link_recipe.hpp"

#include "platform/environment.hpp"
#include "platform/executable.hpp"
#include "platform/file_names.hpp"
#include "platform/process.hpp"
#include "platform/temporary_directory.hpp"
#include "quidra/abi/symbols.hpp"
#include "toolchain/cuda_discovery.hpp"
#include "toolchain/installation_layout.hpp"
#include "toolchain/link_flags.hpp"
#include "toolchain/llvm_discovery.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

using platform::NativeText;

std::vector<std::string> native_source_language_flags(const fs::path& source) {
    const auto original_extension = source.extension().string();
    auto extension = original_extension;
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (extension == ".c") return {"-x", "c", "-std=c17"};
    if (extension == ".cc" || extension == ".cpp" ||
        extension == ".cxx" || extension == ".c++") {
        return {"-x", "c++", "-std=c++20"};
    }
#ifdef __APPLE__
    if (extension == ".mm")
        return {"-x", "objective-c++", "-std=c++20"};
#endif
    if (original_extension == ".S") return {"-x", "assembler-with-cpp"};
    if (extension == ".s") return {"-x", "assembler"};
    return {};
}

// Plain assembly is not preprocessed, so the driver writes no depfile for it
// and warns that -MD and -MF are unused.
bool preprocessed_native_source(const fs::path& source) {
    if (cuda_native_source(source)) return true;
    const auto flags = native_source_language_flags(source);
    return !(flags.size() == 2 && flags[1] == "assembler");
}

std::vector<std::string> split_native_flags(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    bool single = false;
    bool quoted = false;
    bool escaped = false;
    const auto flush = [&]() {
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    };
    for (const char ch : text) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\' && !single) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !quoted) {
            single = !single;
            continue;
        }
        if (ch == '"' && !single) {
            quoted = !quoted;
            continue;
        }
        if (!single && !quoted &&
            (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    if (escaped || single || quoted)
        throw std::runtime_error("pkg-config returned malformed quoting");
    flush();
    return result;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

// Runs one tool. Without a recording its stderr is this process's stderr.
// With one, the tool writes its stderr into a scratch file, whose bytes are
// then appended to the toolchain log and written to this process's stderr.
int run_tool(
    const fs::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<fs::path>& stdout_path,
    const LinkRecording* recording) {
    if (!recording) {
        return platform::run_native_program(program, arguments, stdout_path);
    }
    const auto scratch = recording->directory / "tool.stderr";
    const int status =
        platform::run_native_program(program, arguments, stdout_path, scratch);
    const auto text = read_bytes(scratch);
    std::error_code ignored;
    fs::remove(scratch, ignored);
    if (!text.empty()) {
        std::ofstream log(
            recording->toolchain_log, std::ios::binary | std::ios::app);
        log << text;
        if (!log) {
            throw std::runtime_error(
                "cannot write toolchain log: " + recording->toolchain_log.string());
        }
        platform::write_standard_error(text);
    }
    return status;
}

std::vector<NativeText> native_arguments(const std::vector<std::string>& arguments) {
    std::vector<NativeText> result;
    result.reserve(arguments.size());
    for (const auto& argument : arguments) result.push_back(platform::native_text(argument));
    return result;
}

std::vector<std::string> pkg_config_query(
    const std::vector<std::string>& modules,
    std::string_view option,
    const LinkRecording* recording) {
    if (modules.empty()) return {};
    const auto configured = platform::environment_value("QUIDRA_PKG_CONFIG");
    const std::string program =
        configured && !configured->empty() ? *configured : "pkg-config";
    if (!platform::command_available(program.c_str()) &&
        !fs::is_regular_file(fs::path(program))) {
        throw std::runtime_error(
            "package native dependencies require pkg-config; install it or set QUIDRA_PKG_CONFIG");
    }

    const auto base = fs::temp_directory_path();
    std::random_device random;
    const auto output = base /
        ("quidra-pkg-config-" + std::to_string(random()) + ".txt");
    std::vector<std::string> arguments;
    arguments.emplace_back(option);
    arguments.insert(arguments.end(), modules.begin(), modules.end());
    const int status =
        run_tool(fs::path(program), native_arguments(arguments), output, recording);
    const auto contents = read_bytes(output);
    std::error_code error;
    fs::remove(output, error);
    if (status != 0) {
        throw std::runtime_error(
            "pkg-config failed for package native dependencies");
    }
    return split_native_flags(contents);
}

std::string utf8(const NativeText& text) {
    const auto encoded = fs::path(text).u8string();
    return std::string(encoded.begin(), encoded.end());
}

// One template line: "=" and the literal text, with "\" and line breaks
// escaped, or "@" and a placeholder.
std::string template_literal(std::string_view text) {
    std::string out = "=";
    for (const char ch : text) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out.push_back(ch);
    }
    return out;
}

} // namespace

bool cuda_native_source(const fs::path& source) {
    auto extension = source.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".cu";
}

bool directly_compilable_native_source(const fs::path& source) {
    return cuda_native_source(source) ||
           !native_source_language_flags(source).empty();
}

NativeStep native_source_compile_step(
    const fs::path& source,
    const fs::path& object,
    bool optimize,
    const std::vector<std::string>& extra_compile_flags) {
    std::vector<std::string> arguments;
    arguments.emplace_back(optimize ? "-O2" : "-O0");
    const bool cuda = cuda_native_source(source);
    if (cuda) {
        arguments.emplace_back("-std=c++20");
#ifndef _WIN32
        arguments.emplace_back("-Xcompiler=-fPIC");
#endif
    } else {
#ifndef _WIN32
        arguments.emplace_back("-fPIC");
#endif
        const auto language_flags = native_source_language_flags(source);
        if (language_flags.empty()) {
            throw std::runtime_error(
                "unsupported package native source type: " + source.string());
        }
        arguments.insert(
            arguments.end(), language_flags.begin(), language_flags.end());
    }
    if (const auto include = native_extension_include_directory()) {
        arguments.emplace_back("-I" + include->string());
    }
    arguments.insert(
        arguments.end(), extra_compile_flags.begin(), extra_compile_flags.end());
    arguments.emplace_back("-c");
    arguments.emplace_back(fs::absolute(source).lexically_normal().string());
    arguments.emplace_back("-o");
    arguments.emplace_back(object.string());

    NativeStep step;
    step.kind = NativeStep::Kind::compile;
    step.program = fs::path(cuda ? cuda_driver() : clang_driver());
    step.arguments = native_arguments(arguments);
    step.source = source;
    step.output = object;
    return step;
}

std::vector<std::string> pkg_config_flags(
    const std::vector<std::string>& modules, std::string_view option) {
    return pkg_config_query(modules, option, nullptr);
}

NativeLinkRecipe::NativeLinkRecipe() = default;
NativeLinkRecipe::NativeLinkRecipe(NativeLinkRecipe&&) noexcept = default;
NativeLinkRecipe& NativeLinkRecipe::operator=(NativeLinkRecipe&&) noexcept = default;
NativeLinkRecipe::~NativeLinkRecipe() = default;

std::optional<fs::path> NativeLinkRecipe::work_directory() const {
    if (recording_) return recording_->directory;
    if (temporary_) return temporary_->path();
    return std::nullopt;
}

NativeLinkRecipe native_link_recipe(
    const fs::path& llvm,
    const fs::path& output,
    const LinkOptions& options,
    const std::optional<LinkRecording>& recording) {
    // Every field of LinkOptions, by name: a new field fails to bind here
    // until the recipe decides what it means.
    const auto& [debug, optimize, inputs, pkg_config_modules] = options;

    NativeLinkRecipe recipe;
    recipe.recording_ = recording;
    recipe.llvm_ = llvm;
    recipe.output_ = output;
    const LinkRecording* record = recording ? &*recording : nullptr;
    if (record) {
        std::ofstream log(record->toolchain_log, std::ios::binary | std::ios::trunc);
        if (!log) {
            throw std::runtime_error(
                "cannot write toolchain log: " + record->toolchain_log.string());
        }
    }

    const auto pkg_compile_flags =
        pkg_config_query(pkg_config_modules, "--cflags", record);
    const auto pkg_link_flags =
        pkg_config_query(pkg_config_modules, "--libs", record);

    // Package-owned C/C++ sources are compiled as translation units before the
    // final LLVM link. This gives native packages a stable language standard
    // independent of the host compiler default and keeps mixed C/C++ packages
    // valid. Prebuilt objects/libraries continue through unchanged.
    std::vector<fs::path> link_inputs;
    link_inputs.reserve(inputs.size());
    std::size_t source_index = 0;
    bool uses_cuda = false;
    for (const auto& input : inputs) {
        if (!directly_compilable_native_source(input)) {
            link_inputs.push_back(input);
            continue;
        }
        if (!record && !recipe.temporary_) {
            recipe.temporary_ = std::make_unique<platform::TemporaryDirectory>(
                "quidra-jit-native-",
                "cannot create temporary directory for JIT native sources");
        }
        const auto directory = *recipe.work_directory();
        uses_cuda = uses_cuda || cuda_native_source(input);
        const auto stem = "native-" + std::to_string(source_index++);
        const auto object = directory / platform::object_file_name(stem);
        auto step = native_source_compile_step(
            input, object, optimize && !debug, pkg_compile_flags);
        if (record && preprocessed_native_source(input)) {
            const auto depfile = directory / (stem + ".d");
            step.recording_arguments = {
                platform::native_text("-MD"), platform::native_text("-MF"),
                depfile.native()};
            step.depfile = depfile;
        }
        recipe.steps_.push_back(std::move(step));
        link_inputs.push_back(object);
    }

    NativeStep link;
    link.kind = NativeStep::Kind::link;
    link.output = output;
    link.required_inputs = link_inputs;
#ifdef _WIN32
    std::vector<NativeText> arguments{
        (debug || !optimize) ? L"-O0" : L"-O3",
        L"-fms-runtime-lib=dll",
        L"-Xlinker",
        L"/NODEFAULTLIB:libcmt",
        L"-Wno-override-module",
        L"-x",
        L"ir",
        llvm.native(),
        L"-x",
        L"none",
        runtime_library().native(),
    };
    if (const auto include = native_extension_include_directory()) {
        arguments.emplace_back(L"-I" + include->native());
    }
    for (const auto& flag : pkg_compile_flags)
        arguments.emplace_back(platform::native_text(flag));
    for (const auto& input : link_inputs) arguments.push_back(input.native());
    arguments.insert(arguments.end(), {
        L"-Xlinker",
        L"/DEFAULTLIB:legacy_stdio_definitions",
        L"-o",
        output.native(),
    });
    if (debug) {
        arguments.emplace_back(L"-g");
        arguments.emplace_back(L"-fno-omit-frame-pointer");
    } else if (optimize) {
        arguments.emplace_back(L"-Xlinker");
        arguments.emplace_back(L"/OPT:REF");
    }
#ifdef QUIDRA_SANITIZE_GENERATED
    arguments.emplace_back(L"-fsanitize=address,undefined");
    arguments.emplace_back(L"-fno-omit-frame-pointer");
#endif
    for (const auto& flag : pkg_link_flags)
        arguments.emplace_back(platform::native_text(flag));
    if (uses_cuda) {
        const auto cuda_library = cuda_runtime_library_directory();
        arguments.emplace_back(L"-L" + cuda_library.native());
        arguments.emplace_back(L"-lcudart");
    }
    if (llvm_uses_http(llvm)) arguments.emplace_back(L"-lcurl");
#else
    std::vector<NativeText> arguments{
        (debug || !optimize) ? "-O0" : "-O3",
        "-Wno-override-module",
        "-x",
        "ir",
        llvm.string(),
        "-x",
        "none",
        runtime_library().string(),
    };
    // Linking LLVM IR does not compile source headers. Passing -I here only
    // produces an unused-argument warning with Clang on macOS, polluting
    // command output. Native package sources get -I in their compile steps.
    arguments.insert(
        arguments.end(), pkg_compile_flags.begin(), pkg_compile_flags.end());
    std::vector<fs::path> runtime_library_dirs;
    for (const auto& input : link_inputs) {
        arguments.push_back(input.string());
        const auto filename = input.filename().string();
        const bool shared =
#ifdef __APPLE__
            input.extension() == ".dylib";
#else
            input.extension() == ".so" || filename.find(".so.") != std::string::npos;
#endif
        if (shared) {
            const auto directory = fs::absolute(input.parent_path()).lexically_normal();
            if (std::find(runtime_library_dirs.begin(), runtime_library_dirs.end(),
                          directory) == runtime_library_dirs.end()) {
                runtime_library_dirs.push_back(directory);
            }
        }
    }
    for (const auto& directory : runtime_library_dirs) {
        arguments.push_back("-Wl,-rpath," + directory.string());
    }
    arguments.insert(arguments.end(), {
        "-o",
        output.string(),
    });
    for (const auto& flag : system_link_libraries()) arguments.push_back(flag);
    if (debug) {
        arguments.emplace_back("-g");
        arguments.emplace_back("-fno-omit-frame-pointer");
    } else if (optimize) {
#ifdef __APPLE__
        arguments.emplace_back("-Wl,-dead_strip");
#else
        arguments.emplace_back("-Wl,--gc-sections");
#endif
    }
#ifdef QUIDRA_SANITIZE_GENERATED
    arguments.emplace_back("-fsanitize=address,undefined");
    arguments.emplace_back("-fno-omit-frame-pointer");
#endif
    for (const auto& flag : dependency_link_flags(pkg_link_flags, uses_cuda, llvm_uses_http(llvm)))
        arguments.push_back(flag);
    if (record) {
        // The linker prints the trace on its stdout.
#ifdef __APPLE__
        link.recording_arguments.emplace_back("-Wl,-t");
#else
        link.recording_arguments.emplace_back("-Wl,--trace");
#endif
        link.trace = record->directory / "link.trace";
    }
#endif
    link.arguments = std::move(arguments);
    link.program = fs::path(clang_driver());
    recipe.steps_.push_back(std::move(link));
    return recipe;
}

int NativeLinkRecipe::execute() const {
    const LinkRecording* record = recording_ ? &*recording_ : nullptr;
    for (const auto& step : steps_) {
        auto arguments = step.arguments;
        if (record) {
            arguments.insert(
                arguments.end(), step.recording_arguments.begin(),
                step.recording_arguments.end());
        }
        const auto stdout_path = record ? step.trace : std::nullopt;
        if (step.kind == NativeStep::Kind::compile) {
            if (!fs::is_regular_file(step.source)) {
                throw std::runtime_error(
                    "JIT native source is not a regular file: " + step.source.string());
            }
            if (run_tool(step.program, arguments, stdout_path, record) != 0 ||
                !fs::is_regular_file(step.output)) {
                throw std::runtime_error(
                    "failed to compile package native source: " + step.source.string());
            }
            continue;
        }
        for (const auto& input : step.required_inputs) {
            if (!fs::is_regular_file(input))
                throw std::runtime_error("--link input is not a regular file: " + input.string());
        }
        return run_tool(step.program, arguments, stdout_path, record);
    }
    throw std::runtime_error("native link recipe has no link step");
}

std::string NativeLinkRecipe::template_text() const {
    const auto work = work_directory();
    std::string work_prefix;
    if (work) {
        work_prefix = utf8(work->native());
        if (!work_prefix.empty() && work_prefix.back() != '/' &&
            work_prefix.back() != static_cast<char>(fs::path::preferred_separator)) {
            work_prefix.push_back(static_cast<char>(fs::path::preferred_separator));
        }
    }
    const auto llvm = utf8(llvm_.native());
    const auto output = utf8(output_.native());

    std::string out;
    const auto argument_line = [&](const NativeText& native) {
        const auto text = utf8(native);
        if (text == llvm) return std::string("@llvm");
        if (text == output) return std::string("@output");
        if (!work_prefix.empty() && text.size() > work_prefix.size() &&
            text.compare(0, work_prefix.size(), work_prefix) == 0) {
            return "@work/" +
                   fs::path(text.substr(work_prefix.size())).generic_string();
        }
        return template_literal(text);
    };
    for (const auto& step : steps_) {
        out += step.kind == NativeStep::Kind::compile ? "compile " : "link ";
        out += template_literal(utf8(step.program.native()));
        out += "\n";
        for (const auto& argument : step.arguments) {
            out += "  ";
            out += argument_line(argument);
            out += "\n";
        }
    }
    return out;
}

} // namespace quidra::toolchain
