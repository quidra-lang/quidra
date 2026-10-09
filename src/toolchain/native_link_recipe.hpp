#pragma once

// The native link as a recipe: every command that turns a program's LLVM IR,
// the runtime library and the link inputs into an executable, described in
// full before the first one runs.
//
// native_link_recipe() resolves what the link depends on (the clang driver,
// nvcc for .cu sources, the runtime library, the native extension include
// directory, the CUDA runtime, pkg-config's flags, and whether the program
// calls the HTTP runtime) and lists the steps: one compile per package-owned
// native source, in input order, then the final link. execute() runs them in
// that order with the checks and error messages of the build itself.
//
// A recipe is made in one of two modes:
// - build (no LinkRecording): exactly the commands `quidra build` runs, with
//   the objects in a fresh temporary directory removed with the recipe;
// - run (a LinkRecording): the same commands, which additionally record what
//   a cached run needs to know about the toolchain. Each native compile
//   writes a depfile (-MD -MF) of the headers it read, the final link writes
//   the trace of the files the linker loaded (ld64 -t, GNU ld and lld
//   --trace; none on Windows yet), and the stderr of every tool, pkg-config
//   included, is captured into the toolchain log and still reaches this
//   process's stderr, byte for byte, after the tool exits. The objects,
//   depfiles and the trace live in the recording directory.
// The run-mode additions change no produced byte: an executable linked in
// run mode equals the one build mode links under the same file name.
//
// template_text() is the recipe as text, independent of the mode and of the
// directories it builds in: the work directory, the LLVM file and the output
// become placeholders, and the run-mode additions are left out.

#include "platform/native_text.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::platform {
class TemporaryDirectory;
}

namespace quidra::toolchain {

// What `quidra build`, `quidra run` and `quidra debug` ask of the link.
// native_link_recipe() reads every field through a structured binding, so a
// new field stops the build until the recipe (and every consumer of the
// recipe, such as a cache key) says what it does with it.
struct LinkOptions {
    bool debug{};
    bool optimize{true};
    // Package native libraries and sources, then the --link files, in order.
    std::vector<std::filesystem::path> inputs;
    std::vector<std::string> pkg_config_modules;
};

// Run mode: where the recorded files go. `directory` must exist; the recipe
// writes native-N objects and depfiles, the link trace and scratch files
// there. `toolchain_log` is created empty when the recipe is made.
struct LinkRecording {
    std::filesystem::path directory;
    std::filesystem::path toolchain_log;
};

struct NativeStep {
    enum class Kind { compile, link };

    Kind kind{Kind::compile};
    // The driver: clang (or nvcc for a .cu source) as discovery names it.
    std::filesystem::path program;
    // The arguments every mode passes.
    std::vector<platform::NativeText> arguments;
    // Run mode only, passed after `arguments`.
    std::vector<platform::NativeText> recording_arguments;
    // compile: the native source; link: unused.
    std::filesystem::path source;
    // compile: the object; link: the executable.
    std::filesystem::path output;
    // link: the inputs that must be regular files when the link starts (the
    // compiled objects among them).
    std::vector<std::filesystem::path> required_inputs;
    // Run mode: the compile's depfile, and the link's trace (its stdout).
    std::optional<std::filesystem::path> depfile;
    std::optional<std::filesystem::path> trace;
};

class NativeLinkRecipe {
public:
    NativeLinkRecipe();
    NativeLinkRecipe(NativeLinkRecipe&&) noexcept;
    NativeLinkRecipe& operator=(NativeLinkRecipe&&) noexcept;
    ~NativeLinkRecipe();

    const std::vector<NativeStep>& steps() const { return steps_; }
    const std::optional<LinkRecording>& recording() const { return recording_; }
    // Where the objects go: the recording directory in run mode, else the
    // temporary directory (nullopt when there is nothing to compile).
    std::optional<std::filesystem::path> work_directory() const;

    // Runs the steps in order and returns the final link's exit status.
    // Throws std::runtime_error, with the build's messages, when a native
    // source or a link input is not a regular file or a native compile
    // fails.
    int execute() const;

    std::string template_text() const;

private:
    friend NativeLinkRecipe native_link_recipe(
        const std::filesystem::path&, const std::filesystem::path&,
        const LinkOptions&, const std::optional<LinkRecording>&);

    std::vector<NativeStep> steps_;
    std::optional<LinkRecording> recording_;
    std::unique_ptr<platform::TemporaryDirectory> temporary_;
    std::filesystem::path llvm_;
    std::filesystem::path output_;
};

NativeLinkRecipe native_link_recipe(
    const std::filesystem::path& llvm,
    const std::filesystem::path& output,
    const LinkOptions& options,
    const std::optional<LinkRecording>& recording = std::nullopt);

// Pieces the REPL's JIT shares with the recipe.

// Whether a link input is a .cu source (compiled by nvcc).
bool cuda_native_source(const std::filesystem::path& source);
// Whether a link input is a native source the build compiles (C, C++,
// Objective-C++ on Apple platforms, assembly, CUDA) rather than an object or
// library it passes to the linker.
bool directly_compilable_native_source(const std::filesystem::path& source);

// The compile of one package-owned native source into `object`: -O2 (or -O0
// without `optimize`), the language flags of its extension, the native
// extension include directory and `extra_compile_flags`. Throws when the
// source type is unsupported or the driver is not found.
NativeStep native_source_compile_step(
    const std::filesystem::path& source,
    const std::filesystem::path& object,
    bool optimize,
    const std::vector<std::string>& extra_compile_flags = {});

// pkg-config's output for `option` (--cflags or --libs) over `modules`,
// split into arguments; empty without modules. QUIDRA_PKG_CONFIG names the
// program, else pkg-config on PATH.
std::vector<std::string> pkg_config_flags(
    const std::vector<std::string>& modules, std::string_view option);

} // namespace quidra::toolchain
