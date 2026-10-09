#include "platform/native_text.hpp"
#include "platform/temporary_directory.hpp"
#include "toolchain/native_link_recipe.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

// The native link recipe (src/toolchain/native_link_recipe.hpp) with the
// host's real toolchain: a C source with a local header, a plain assembly
// source and a minimal LLVM module, linked against the runtime library.
// - Build mode adds nothing: no depfile, no trace, no recording arguments.
// - Run mode records a depfile per preprocessed native compile (none for
//   plain assembly), the link trace (POSIX) and the tools' stderr, which it
//   also relays to this process's stderr unchanged.
// - Both modes have the same template, and the executables they link under
//   the same file name are byte-identical.
// - execute() reports a missing native source or link input with the build's
//   messages.

namespace fs = std::filesystem;
using quidra::toolchain::LinkOptions;
using quidra::toolchain::LinkRecording;
using quidra::toolchain::NativeLinkRecipe;
using quidra::toolchain::NativeStep;

namespace {

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "native link recipe test failed: %s\n", what.c_str());
        ++failures;
    }
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string text(const quidra::platform::NativeText& native) {
    const auto encoded = fs::path(native).u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::vector<std::string> texts(const std::vector<quidra::platform::NativeText>& natives) {
    std::vector<std::string> out;
    for (const auto& native : natives) out.push_back(text(native));
    return out;
}

std::string message_of(const NativeLinkRecipe& recipe) {
    try {
        (void)recipe.execute();
    } catch (const std::exception& error) {
        return error.what();
    }
    return "";
}

// Runs `action` with this process's stderr sent to `path` (POSIX only).
template <class Action>
void with_stderr_in(const fs::path& path, Action action) {
#ifdef _WIN32
    (void)path;
    action();
#else
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (saved < 0 || file < 0 || ::dup2(file, STDERR_FILENO) < 0)
        throw std::runtime_error("cannot redirect stderr");
    ::close(file);
    try {
        action();
    } catch (...) {
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
        throw;
    }
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
#endif
}

void test_modes(const fs::path& root) {
    write_text(root / "recipe_header.h", "#define RECIPE_VALUE 7\n");
    write_text(
        root / "native.c",
        "#include \"recipe_header.h\"\n"
        "int recipe_native_value(void) { return RECIPE_VALUE; }\n");
    write_text(root / "plain.s", "\t.text\n");
    write_text(root / "program.ll", "define i32 @main() {\n  ret i32 0\n}\n");
    const auto llvm = root / "program.ll";
    const LinkOptions options{
        false, true, {root / "native.c", root / "plain.s"}, {}};

    fs::create_directories(root / "build");
    const auto built = quidra::toolchain::native_link_recipe(
        llvm, root / "build" / "app", options);
    fs::create_directories(root / "run");
    const LinkRecording recording{root / "run", root / "run" / "toolchain.log"};
    const auto recorded = quidra::toolchain::native_link_recipe(
        llvm, root / "run" / "app", options, recording);

    expect(!built.recording(), "build mode has no recording");
    expect(built.steps().size() == 3, "build mode: two compiles and a link");
    expect(recorded.steps().size() == 3, "run mode: two compiles and a link");
    for (const auto& step : built.steps()) {
        expect(step.recording_arguments.empty(), "build mode adds no arguments");
        expect(!step.depfile && !step.trace, "build mode records nothing");
    }
    expect(fs::is_regular_file(recording.toolchain_log),
           "run mode creates the toolchain log");

    const auto template_text = built.template_text();
    expect(template_text == recorded.template_text(),
           "the template does not depend on the mode or the directories:\n" +
               template_text + "---\n" + recorded.template_text());
    expect(contains(template_text, "\n  =-x\n  =ir\n  @llvm\n"),
           "the template names the LLVM file by placeholder:\n" + template_text);
    expect(contains(template_text, "  =-o\n  @output\n"),
           "the template names the output by placeholder:\n" + template_text);
    expect(contains(template_text, "@work/native-0."),
           "the template names objects by placeholder:\n" + template_text);
    expect(!contains(template_text, "-MD") && !contains(template_text, "trace") &&
               !contains(template_text, "=-Wl,-t\n"),
           "the template leaves out the recording:\n" + template_text);

    const auto& c_compile = recorded.steps()[0];
    expect(c_compile.kind == NativeStep::Kind::compile, "the C source compiles first");
    expect(c_compile.depfile && *c_compile.depfile == root / "run" / "native-0.d",
           "the C compile writes a depfile in the recording directory");
    const auto c_extra = texts(c_compile.recording_arguments);
    expect(c_extra.size() == 3 && c_extra[0] == "-MD" && c_extra[1] == "-MF",
           "the C compile adds -MD -MF");
    const auto& asm_compile = recorded.steps()[1];
    expect(!asm_compile.depfile && asm_compile.recording_arguments.empty(),
           "plain assembly gets no depfile");
    const auto& link = recorded.steps()[2];
    expect(link.kind == NativeStep::Kind::link, "the link comes last");
#ifdef _WIN32
    expect(!link.trace, "no link trace on Windows yet");
#else
    expect(link.trace && *link.trace == root / "run" / "link.trace",
           "the link writes its trace in the recording directory");
    expect(link.recording_arguments.size() == 1, "the link adds one trace flag");
#endif

    expect(built.execute() == 0, "build mode links");
    std::optional<int> recorded_status;
    with_stderr_in(root / "relayed.txt", [&] { recorded_status = recorded.execute(); });
    expect(recorded_status == 0, "run mode links");
    expect(read_bytes(root / "build" / "app") == read_bytes(root / "run" / "app"),
           "run mode links the same bytes as build mode");
    const auto depfile = read_bytes(root / "run" / "native-0.d");
    expect(contains(depfile, "recipe_header.h"),
           "the depfile lists the local header:\n" + depfile);
    expect(!fs::exists(root / "run" / "native-1.d"), "no depfile for plain assembly");
#ifndef _WIN32
    expect(contains(read_bytes(root / "run" / "link.trace"), "native-0"),
           "the link trace lists the compiled object");
    const auto log = read_bytes(recording.toolchain_log);
    expect(read_bytes(root / "relayed.txt") == log,
           "the relayed stderr equals the toolchain log");
    expect(!contains(log, "-MD") && !contains(log, "-MF"),
           "the recording causes no warning:\n" + log);
#endif
    expect(!fs::exists(root / "run" / "tool.stderr"), "no scratch file is left");
}

void test_captured_warning(const fs::path& root) {
    write_text(root / "warn.c", "#warning \"recipe warning\"\nint recipe_warn;\n");
    write_text(root / "program.ll", "define i32 @main() {\n  ret i32 0\n}\n");
    const LinkRecording recording{root, root / "toolchain.log"};
    const auto recipe = quidra::toolchain::native_link_recipe(
        root / "program.ll", root / "app",
        LinkOptions{false, true, {root / "warn.c"}, {}}, recording);
    std::optional<int> status;
    with_stderr_in(root / "relayed.txt", [&] { status = recipe.execute(); });
    expect(status == 0, "a warning does not fail the link");
    const auto log = read_bytes(recording.toolchain_log);
    expect(contains(log, "recipe warning"), "the log holds the warning:\n" + log);
#ifndef _WIN32
    expect(read_bytes(root / "relayed.txt") == log,
           "the warning reaches stderr unchanged");
#endif
}

void test_missing_files(const fs::path& root) {
    write_text(root / "program.ll", "define i32 @main() {\n  ret i32 0\n}\n");
    const auto missing_source = root / "gone.c";
    const auto source_recipe = quidra::toolchain::native_link_recipe(
        root / "program.ll", root / "app", LinkOptions{false, true, {missing_source}, {}});
    expect(message_of(source_recipe) ==
               "JIT native source is not a regular file: " + missing_source.string(),
           "a missing native source keeps its message");
    const auto missing_input = root / "missing.o";
    const auto input_recipe = quidra::toolchain::native_link_recipe(
        root / "program.ll", root / "app", LinkOptions{false, true, {missing_input}, {}});
    expect(message_of(input_recipe) ==
               "--link input is not a regular file: " + missing_input.string(),
           "a missing link input keeps its message");
}

} // namespace

int main() {
    try {
        const quidra::platform::TemporaryDirectory modes(
            "quidra-recipe-modes-", "cannot create a test directory");
        test_modes(modes.path());
        const quidra::platform::TemporaryDirectory warning(
            "quidra-recipe-warning-", "cannot create a test directory");
        test_captured_warning(warning.path());
        const quidra::platform::TemporaryDirectory missing(
            "quidra-recipe-missing-", "cannot create a test directory");
        test_missing_files(missing.path());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native link recipe test failed: %s\n", error.what());
        return 1;
    }
    if (failures) return 1;
    std::puts("native link recipe tests passed");
    return 0;
}
