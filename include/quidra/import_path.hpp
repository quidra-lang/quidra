#pragma once

#include "quidra/language.hpp"
#include "quidra/project.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra {

class CompileInputs;

inline constexpr bool is_importable_package_name(std::string_view name) {
    if (name.empty()) return false;
    const auto ascii_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    const auto ascii_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!ascii_alpha(name.front()) && name.front() != '_') return false;
    for (const char c : name.substr(1)) {
        if (!ascii_alpha(c) && !ascii_digit(c) && c != '_') return false;
    }

    // These spellings are tokenized as language keywords and therefore cannot
    // appear as the unquoted installed-package target in an import declaration.
    if (name == "class" || name == "import" || name == "const" || name == "return" ||
        name == "if" || name == "then" || name == "elif" || name == "else" ||
        name == "while" || name == "for" || name == "in" ||
        name == "match" || name == "try" || name == "break" ||
        name == "continue" || name == "this" || name == "true" || name == "false" ||
        name == "not" || name == "and" || name == "or") {
        return false;
    }

    return !is_standard_module(name);
}

enum class ImportPathBase {
    ImporterDirectory,
    CommandWorkingDirectory
};

struct ImportPathResolution {
    ImportPathBase base{ImportPathBase::ImporterDirectory};
    std::filesystem::path path;
};

inline bool import_path_uses_command_root(std::string_view source) {
    return source.starts_with("@/");
}

inline ImportPathResolution resolve_local_import_path(
    std::string_view source,
    const std::filesystem::path& importer_file,
    const std::filesystem::path& command_working_directory) {
    if (source.empty()) {
        throw std::invalid_argument("Import path cannot be empty.");
    }

    if (source.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Import path cannot contain NUL.");
    }

    const auto cwd = std::filesystem::absolute(command_working_directory).lexically_normal();
    const auto importer = importer_file.is_absolute()
        ? importer_file.lexically_normal()
        : (cwd / importer_file).lexically_normal();

    ImportPathBase base = ImportPathBase::ImporterDirectory;
    std::filesystem::path relative;

    if (import_path_uses_command_root(source)) {
        base = ImportPathBase::CommandWorkingDirectory;
        const auto remainder = source.substr(2);
        if (remainder.empty()) {
            throw std::invalid_argument(
                "Project-root import path must name a " +
                std::string(source_extension) + " file.");
        }
        if (remainder.front() == '/' || remainder.front() == '\\') {
            throw std::invalid_argument(
                "Project-root import path must remain relative after '@/'.");
        }

        relative = std::filesystem::path(std::string(remainder));
        if (relative.is_absolute()) {
            throw std::invalid_argument("Project-root import path must remain relative after '@/'.");
        }

        const auto normalized = relative.lexically_normal();
        if (!normalized.empty() && *normalized.begin() == "..") {
            throw std::invalid_argument("Project-root import path cannot escape the command working directory.");
        }
        relative = normalized;
    } else {
        relative = std::filesystem::path(std::string(source));
        if (relative.is_absolute()) {
            throw std::invalid_argument("Local import path must be relative.");
        }
        relative = relative.lexically_normal();
    }

    if (relative.extension() != source_extension) {
        throw std::invalid_argument(
            "Local import path must use the " +
            std::string(source_extension) + " extension.");
    }

    const auto base_path =
        base == ImportPathBase::CommandWorkingDirectory ? cwd : importer.parent_path();

    return ImportPathResolution{base, (base_path / relative).lexically_normal()};
}

// The entrypoint of installed package `package_name`: the first root of
// QUIDRA_PACKAGE_PATH that holds it, else the user package store. Defined in
// src/import_path.cpp, which reads the environment. With `inputs`, every
// candidate probed without success and the resolution itself are recorded.
std::optional<std::filesystem::path> resolve_installed_package_path(
    std::string_view package_name, CompileInputs* inputs = nullptr);

} // namespace quidra
