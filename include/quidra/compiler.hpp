#pragma once
#include "quidra/checker.hpp"
#include "quidra/ir.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace quidra {

struct Compilation {
    CheckedProgram checked;
    ir::Module ir;
    std::string llvm;
};

struct CompileOptions {
    std::size_t max_errors{20};
    bool debug_info{};
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
Compilation compile_file(
    const std::filesystem::path& source,
    CompileOptions options = {},
    const std::filesystem::path& command_working_directory = std::filesystem::current_path());

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
