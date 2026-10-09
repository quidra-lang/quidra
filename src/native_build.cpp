#include "native_build.hpp"
#include "quidra/package_manifest.hpp"
#include "platform/environment.hpp"
#include "platform/executable.hpp"
#include "platform/file_names.hpp"
#include "platform/process.hpp"
#include "platform/temporary_directory.hpp"
#include "platform/native_text.hpp"
#include "toolchain/archiver.hpp"
#include "toolchain/cuda_discovery.hpp"
#include "toolchain/installation_layout.hpp"
#include "toolchain/link_flags.hpp"
#include "toolchain/llvm_discovery.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::native {

PackageNativeBuildInputs package_native_build_inputs(
    const std::map<std::string, fs::path>& packages, CompileInputs* inputs) {
    PackageNativeBuildInputs result;
    for (const auto& [name, package_main] : packages) {
        (void)name;
        const auto root = package_main.parent_path();
        const auto manifest = quidra::try_read_package_manifest(root, inputs);
        if (!manifest) continue;
        if (const auto native = quidra::package_native_library_path(root, *manifest)) {
            result.inputs.push_back(PackageNativeInput{*native, false});
        }
        for (auto& source : quidra::package_native_source_paths(root, *manifest)) {
            result.inputs.push_back(PackageNativeInput{std::move(source), true});
        }
        for (const auto& [_, module] : manifest->native_pkg_config)
            result.pkg_config_modules.push_back(module);
    }
    return result;
}

class TemporaryJitNativeObjects {
public:
    TemporaryJitNativeObjects()
        : directory_("quidra-jit-native-",
                     "cannot create temporary directory for JIT native sources") {}

    fs::path object(std::size_t index) const {
        return directory_.path() /
               platform::object_file_name("native-" + std::to_string(index));
    }

    fs::path library() const {
        return directory_.path() / platform::shared_library_file_name("native-package");
    }

private:
    platform::TemporaryDirectory directory_;
};

void compile_jit_native_source(
    const fs::path& source,
    const fs::path& object,
    bool optimize,
    const std::vector<std::string>& extra_compile_flags = {}) {
    if (!fs::is_regular_file(source)) {
        throw std::runtime_error(
            "JIT native source is not a regular file: " + source.string());
    }

    const auto step = toolchain::native_source_compile_step(
        source, object, optimize, extra_compile_flags);
    if (platform::run_native_program(step.program, step.arguments) != 0 ||
        !fs::is_regular_file(object)) {
        throw std::runtime_error(
            "failed to compile package native source: " + source.string());
    }
}

void compile_jit_native_library(
    const std::vector<fs::path>& sources,
    const fs::path& output,
    bool optimize,
    const std::vector<std::string>& compile_flags,
    const std::vector<std::string>& link_flags) {
#ifdef _WIN32
    (void)sources;
    (void)output;
    (void)optimize;
    (void)compile_flags;
    (void)link_flags;
    throw std::runtime_error(
        "pkg-config native dependencies are not yet supported by the Windows REPL JIT; use AOT run/build");
#else
    std::vector<std::string> arguments;
    arguments.emplace_back(optimize ? "-O2" : "-O0");
    arguments.emplace_back("-shared");
    arguments.emplace_back("-fPIC");
#ifdef __APPLE__
    arguments.emplace_back("-Wl,-undefined,dynamic_lookup");
#endif
    if (const auto include = toolchain::native_extension_include_directory())
        arguments.emplace_back("-I" + include->string());

    std::vector<fs::path> objects;
    objects.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (!fs::is_regular_file(source))
            throw std::runtime_error(
                "JIT native source is not a regular file: " + source.string());
#ifdef _WIN32
        const auto object =
            output.parent_path() / ("pkg-" + std::to_string(index) + ".obj");
#else
        const auto object =
            output.parent_path() / ("pkg-" + std::to_string(index) + ".o");
#endif
        compile_jit_native_source(
            source, object, optimize, compile_flags);
        objects.push_back(object);
    }
    for (const auto& object : objects)
        arguments.push_back(fs::absolute(object).lexically_normal().string());
    arguments.insert(arguments.end(), link_flags.begin(), link_flags.end());
    const bool uses_cuda = std::any_of(
        sources.begin(), sources.end(),
        [](const fs::path& source) { return toolchain::cuda_native_source(source); });
    if (uses_cuda) {
        const auto cuda_library = toolchain::cuda_runtime_library_directory();
        arguments.emplace_back("-L" + cuda_library.string());
        arguments.emplace_back("-Wl,-rpath," + cuda_library.string());
        arguments.emplace_back("-lcudart");
    }
    arguments.emplace_back("-o");
    arguments.emplace_back(output.string());
    if (platform::run_program(fs::path(toolchain::clang_driver()), arguments) != 0 ||
        !fs::is_regular_file(output)) {
        throw std::runtime_error(
            "failed to build JIT package native library");
    }
#endif
}

int run_llvm_jit(
    const fs::path& llvm,
    const std::vector<std::string>& arguments,
    JitOptions options,
    const std::optional<fs::path>& stdout_path,
    const std::optional<fs::path>& stderr_path) {
    if (!fs::is_regular_file(llvm)) {
        throw std::runtime_error("JIT input is not a regular LLVM IR file: " + llvm.string());
    }

    const auto driver = toolchain::jit_driver();
    std::vector<std::string> jit_arguments;
    jit_arguments.reserve(arguments.size() + 8);
    // Explicitly select ORC so Quidra never silently falls back to LLVM's
    // interpreter execution mode.
    jit_arguments.emplace_back("--jit-kind=orc");
    for (auto& argument : toolchain::jit_platform_arguments(driver))
        jit_arguments.push_back(std::move(argument));
    jit_arguments.emplace_back(options.optimize ? "-O2" : "-O0");
    jit_arguments.emplace_back("--dlopen=" + toolchain::jit_runtime_library().string());

    for (const auto& library : options.libraries) {
        if (!fs::is_regular_file(library)) {
            throw std::runtime_error(
                "JIT native library is not a regular file: " + library.string());
        }
        jit_arguments.emplace_back(
            "--dlopen=" + fs::absolute(library).lexically_normal().string());
    }

    std::optional<TemporaryJitNativeObjects> native_objects;
    if (!options.sources.empty()) {
        native_objects.emplace();
        const auto compile_flags =
            toolchain::pkg_config_flags(options.pkg_config_modules, "--cflags");
#ifdef _WIN32
        const bool uses_cuda = std::any_of(
            options.sources.begin(), options.sources.end(),
            [](const fs::path& source) { return toolchain::cuda_native_source(source); });
        // Windows still uses direct ORC objects for dependency-free native
        // sources; the shared-library helper does not yet support that host.
        const bool link_native_library =
            !options.pkg_config_modules.empty() || uses_cuda;
#else
        // POSIX package-native C/C++ translation units may contain ordinary
        // platform TLS and C++ runtime relocations that ORC does not support
        // when a raw object is injected with --extra-object. Link them as a
        // shared library so the platform dynamic loader owns TLS/runtime
        // relocation while Core remains unaware of package semantics.
        const bool link_native_library = true;
#endif
        if (!link_native_library) {
            for (std::size_t index = 0; index < options.sources.size(); ++index) {
                const auto object = native_objects->object(index);
                compile_jit_native_source(
                    options.sources[index], object, options.optimize, compile_flags);
                jit_arguments.emplace_back(
                    "--extra-object=" + fs::absolute(object).lexically_normal().string());
            }
        } else {
            const auto library = native_objects->library();
            const auto link_flags =
                toolchain::pkg_config_flags(options.pkg_config_modules, "--libs");
            compile_jit_native_library(
                options.sources, library, options.optimize,
                compile_flags, link_flags);
            jit_arguments.emplace_back(
                "--dlopen=" + fs::absolute(library).lexically_normal().string());
        }
    }

    jit_arguments.emplace_back("--fake-argv0=" + options.argv0);
    jit_arguments.emplace_back(llvm.string());
    jit_arguments.insert(jit_arguments.end(), arguments.begin(), arguments.end());

    return platform::run_program(
        fs::path(driver), jit_arguments, stdout_path, stderr_path);
}

int link_llvm(const fs::path& llvm, const fs::path& output, LinkOptions options) {
    return toolchain::native_link_recipe(llvm, output, options).execute();
}

namespace {

bool static_archive_input(const fs::path& input) {
    const auto extension = input.extension();
    return extension == ".a" || extension == ".lib";
}

bool object_input(const fs::path& input) {
    const auto extension = input.extension();
    return extension == ".o" || extension == ".obj";
}

} // namespace

void build_library(const fs::path& llvm, const fs::path& output, const LinkOptions& options) {
    const auto& [debug, optimize, inputs, pkg_config_modules] = options;
    platform::TemporaryDirectory work("quidra-lib-", "cannot create temporary directory for the library build");
    const auto compile_flags = toolchain::pkg_config_flags(pkg_config_modules, "--cflags");
    std::vector<fs::path> objects;
    std::vector<fs::path> archives;
    // The module, compiled the way the executable link compiles it.
    const auto module_object = work.path() / platform::object_file_name("program");
    std::vector<platform::NativeText> arguments{
        platform::native_text((debug || !optimize) ? "-O0" : "-O3"),
        platform::native_text("-Wno-override-module"),
        platform::native_text("-c"),
        platform::native_text("-x"),
        platform::native_text("ir"),
        llvm.native(),
        platform::native_text("-o"),
        module_object.native()};
    if (debug) arguments.push_back(platform::native_text("-g"));
    if (platform::run_native_program(fs::path(toolchain::clang_driver()), arguments) != 0 ||
        !fs::is_regular_file(module_object))
        throw std::runtime_error("failed to compile the library module");
    objects.push_back(module_object);
    std::size_t source_index = 0;
    for (const auto& input : inputs) {
        if (toolchain::directly_compilable_native_source(input)) {
            const auto object =
                work.path() / platform::object_file_name("native-" + std::to_string(source_index++));
            const auto step = toolchain::native_source_compile_step(
                input, object, optimize && !debug, compile_flags);
            if (!fs::is_regular_file(input))
                throw std::runtime_error("JIT native source is not a regular file: " + input.string());
            if (platform::run_native_program(step.program, step.arguments) != 0 ||
                !fs::is_regular_file(object))
                throw std::runtime_error("failed to compile package native source: " + input.string());
            objects.push_back(object);
        } else if (object_input(input)) {
            objects.push_back(input);
        } else if (static_archive_input(input)) {
            archives.push_back(input);
        } else {
            throw std::runtime_error(
                "a library build cannot include " + input.string() +
                "; only objects, static archives and native sources go into the archive");
        }
    }
    archives.push_back(toolchain::runtime_library());
    toolchain::create_static_archive(output, objects, archives);
}

std::vector<std::string> library_link_flags(const fs::path& llvm, const LinkOptions& options) {
    const auto& [debug, optimize, inputs, pkg_config_modules] = options;
    (void)debug;
    (void)optimize;
    const bool uses_cuda = std::any_of(inputs.begin(), inputs.end(), [](const fs::path& input) {
        return toolchain::cuda_native_source(input);
    });
    return toolchain::c_host_link_flags(toolchain::pkg_config_flags(pkg_config_modules, "--libs"),
                                        uses_cuda, toolchain::llvm_uses_http(llvm));
}


int run_debugger(
    const fs::path& program,
    const std::vector<std::string>& program_arguments) {
    const auto debugger=toolchain::debugger_driver();
    const auto name=fs::path(debugger).filename().string();
    std::vector<platform::NativeText> arguments;
    arguments.push_back(platform::native_text(name.find("gdb") != std::string::npos ? "--args" : "--"));
    arguments.push_back(program.native());
    for (const auto& argument : program_arguments)
        arguments.push_back(platform::native_text(argument));
    return platform::run_native_program(fs::path(debugger),arguments);
}

} // namespace quidra::native
