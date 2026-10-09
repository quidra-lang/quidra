#pragma once
#include "quidra/checker.hpp"
#include "quidra/ir.hpp"
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace quidra {

class CompileInputs;

struct Compilation {
    CheckedProgram checked;
    ir::Module ir;
    std::string llvm;
    // The packages the program imports (ResolvedProgram::packages), so that
    // a native build finds their native inputs without loading the modules
    // again.
    std::map<std::string, std::filesystem::path> packages;
};

struct CompileOptions {
    std::size_t max_errors{20};
    bool debug_info{};
    // The root file as the user spelled it, which runtime failures name
    // (local imports are shown relative to it). Empty: the path the compile
    // was given; string input is "<memory>" and the REPL "<repl>". A path in
    // angle brackets names no file: its sources record no revision, so their
    // failures show no source line.
    std::string source_display_path{};
    // Internal lowering switches for tests (quidra/lowering.hpp).
    ir::LoweringOptions lowering{};
    // What the compilation produces (quidra/checker.hpp). The REPL's
    // compiles are always Interactive.
    CompileArtifact artifact{CompileArtifact::Executable};
};

struct ReplCompilation {
    Compilation compilation;
    std::optional<Type> expression_type;
};

CheckedProgram check(std::string_view source, CompileOptions options = {});
CheckedProgram check_file(
    const std::filesystem::path& source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path());

CheckedProgram check_file_source(
    const std::filesystem::path& source_path,
    std::string_view source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path());

Compilation compile(std::string_view source, CompileOptions options = {});
// With `inputs`, everything the compilation reads from the file system is
// recorded (quidra/compile_inputs.hpp); the compilation is the same.
Compilation compile_file(
    const std::filesystem::path& source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path(),
    CompileInputs* inputs = nullptr);

ReplCompilation compile_repl_file(
    const std::filesystem::path& source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path(),
    std::size_t replay_prefix_bytes = 0);

ReplCompilation compile_repl_file_source(
    const std::filesystem::path& source_path,
    std::string_view source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path(),
    std::size_t replay_prefix_bytes = 0);

} // namespace quidra
