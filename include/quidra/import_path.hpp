#pragma once

#include "quidra/language.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra {

inline std::optional<std::string> import_environment_value(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::nullopt;
    std::string value(raw, size > 0 ? size - 1 : 0);
    std::free(raw);
    return value;
#else
    if (const char* raw = std::getenv(name)) return std::string(raw);
    return std::nullopt;
#endif
}

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
    if (name == "class" || name == "override" || name == "import" ||
        name == "super" || name == "const" || name == "return" ||
        name == "if" || name == "elif" || name == "else" ||
        name == "while" || name == "for" || name == "in" ||
        name == "match" || name == "try" || name == "break" ||
        name == "continue" || name == "true" || name == "false" ||
        name == "not" || name == "and" || name == "or") {
        return false;
    }

    // Standard namespaces are always visible and are deliberately not package
    // imports, so accepting one here would install an unreachable package.
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
            throw std::invalid_argument("Project-root import path must name a .qui file.");
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

    if (relative.extension() != ".qui") {
        throw std::invalid_argument("Local import path must use the .qui extension.");
    }

    const auto base_path =
        base == ImportPathBase::CommandWorkingDirectory ? cwd : importer.parent_path();

    return ImportPathResolution{base, (base_path / relative).lexically_normal()};
}

inline std::optional<std::filesystem::path> resolve_installed_package_path(
    std::string_view package_name) {
    if (!is_importable_package_name(package_name)) {
        throw std::invalid_argument(
            "Package name must be an importable Quidra identifier and must not be a standard namespace.");
    }

    const auto candidate_from_root = [&](const std::filesystem::path& root)
        -> std::optional<std::filesystem::path> {
        if (root.empty()) return std::nullopt;
        std::error_code error;
        const auto candidate =
            (root / std::string(package_name) / "main.qui").lexically_normal();
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
        return std::nullopt;
    };

    if (const auto configured = import_environment_value("QUIDRA_PACKAGE_PATH")) {
        const std::string& paths = *configured;
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::size_t start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(separator, start);
            const auto part = paths.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) {
                if (auto resolved = candidate_from_root(std::filesystem::path(part))) {
                    return resolved;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

#ifdef _WIN32
    const auto home = import_environment_value("USERPROFILE");
#else
    const auto home = import_environment_value("HOME");
#endif
    if (home) {
        if (auto resolved =
                candidate_from_root(std::filesystem::path(*home) / ".quidra" / "packages")) {
            return resolved;
        }
    }
    return std::nullopt;
}

} // namespace quidra
