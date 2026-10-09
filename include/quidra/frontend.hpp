#pragma once

#include "quidra/ast.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace quidra {

class CompileInputs;

// With `inputs`, every file the loader reads and every resolution it makes
// is recorded (quidra/compile_inputs.hpp).
ResolvedProgram load_program_with_modules(
    const std::filesystem::path& root_file,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors = 20,
    CompileInputs* inputs = nullptr);

ResolvedProgram load_program_with_root_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors = 20,
    // Pathless string compilation rejects imports before module loading, so it has
    // no package graph whose lockfile can be meaningfully validated.
    bool enforce_package_lock = true);

std::map<std::string, std::filesystem::path> resolve_package_dependencies(
    const std::filesystem::path& root_file,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors = 20);

std::map<std::string, std::filesystem::path> resolve_package_dependencies_source(
    const std::filesystem::path& root_file,
    std::string_view root_source,
    const std::filesystem::path& command_working_directory,
    std::size_t max_errors = 20);

ConcreteProgram expand_generics(ResolvedProgram program);

} // namespace quidra
