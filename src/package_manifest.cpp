#include "quidra/package_manifest.hpp"

#include "quidra/toml_subset.hpp"
#include "quidra/project.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace quidra {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

unsigned long long parse_component(std::string_view text) {
    if (text.empty()) throw std::runtime_error("semantic version component cannot be empty");
    unsigned long long value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("semantic version components must be decimal integers");
    }
    if (text.size() > 1 && text.front() == '0') {
        throw std::runtime_error("semantic version components cannot contain leading zeroes");
    }
    return value;
}

int compare(const SemanticVersion& left, const SemanticVersion& right) {
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    return 0;
}

bool clause_matches(const VersionClause& clause, const SemanticVersion& candidate_version) {
    const int order = compare(candidate_version, clause.version);
    switch (clause.op) {
        case VersionOperator::Equal: return order == 0;
        case VersionOperator::Less: return order < 0;
        case VersionOperator::LessEqual: return order <= 0;
        case VersionOperator::Greater: return order > 0;
        case VersionOperator::GreaterEqual: return order >= 0;
    }
    return false;
}

std::optional<PackageProject> read_package_project(
    const fs::path& package_root, const PackageManifest& manifest) {
    const auto path =
        package_root / std::string(package_project_filename);
    const auto document = try_read_toml_subset(path);
    if (!document) return std::nullopt;

    const auto required = [&](std::string_view key) -> const std::string& {
        if (const auto* value = document->find("package", key)) return *value;
        throw std::runtime_error("project.toml requires 'package." +
                                 std::string(key) + "': " + path.string());
    };

    PackageProject project;
    project.distribution_name = required("name");
    project.import_name = required("import");
    project.display_name = required("display_name");
    project.repository = required("repository");

    if (!is_distribution_package_name(project.distribution_name)) {
        throw std::runtime_error(
            "project.toml package.name must be a lowercase distribution name "
            "using letters, digits, '-' or '_': " + path.string());
    }

    // project.toml is the source these were generated from, so a disagreement
    // means quidra.package was hand-edited and the two have drifted.
    if (project.import_name != manifest.name) {
        throw std::runtime_error(
            "project.toml package.import '" + project.import_name +
            "' does not match quidra.package name '" + manifest.name +
            "': " + path.string());
    }
    const auto project_version = parse_semantic_version(required("version"));
    if (compare(project_version, manifest.version) != 0) {
        throw std::runtime_error(
            "project.toml package.version " + project_version.str() +
            " does not match quidra.package version " + manifest.version.str() +
            ": " + path.string());
    }
    if (manifest.repository && *manifest.repository != project.repository) {
        throw std::runtime_error(
            "project.toml package.repository '" + project.repository +
            "' does not match quidra.package repository '" +
            *manifest.repository + "': " + path.string());
    }
    if (const auto* q = document->find("requires", "quidra")) {
        const auto found = manifest.requirements.find("quidra");
        if (found == manifest.requirements.end() || found->second.text != *q) {
            throw std::runtime_error(
                "project.toml requires.quidra does not match quidra.package: " +
                path.string());
        }
    }

    if (const auto* abi = document->find("requires", "abi")) {
        project.abi_requirement = parse_component(*abi);
    }

    if (const auto table = document->tables.find("compiler.extension");
        table != document->tables.end()) {
        for (const auto& [name, configured] : table->second) {
            if (name.empty() ||
                std::any_of(name.begin(), name.end(), [](unsigned char ch) {
                    return !(std::isalnum(ch) || ch == '_' || ch == '-');
                })) {
                throw std::runtime_error(
                    "project.toml compiler extension name contains an invalid "
                    "character: " + name + ": " + path.string());
            }
            fs::path relative(configured);
            if (relative.empty() || relative.is_absolute()) {
                throw std::runtime_error(
                    "project.toml compiler extension path must be "
                    "package-relative: " + configured + ": " + path.string());
            }
            for (const auto& component : relative) {
                if (component == "..") {
                    throw std::runtime_error(
                        "project.toml compiler extension path may not escape "
                        "the package root: " + configured + ": " + path.string());
                }
            }
            project.compiler_extensions.emplace(name, configured);
        }
    }
    return project;
}

} // namespace

bool is_distribution_package_name(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') return false;
    for (const char ch : name) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_')) return false;
    }
    return true;
}

std::string_view package_distribution_name(const PackageManifest& manifest) {
    return manifest.project ? std::string_view(manifest.project->distribution_name)
                            : std::string_view(manifest.name);
}

std::string_view package_import_name(const PackageManifest& manifest) {
    return manifest.project ? std::string_view(manifest.project->import_name)
                            : std::string_view(manifest.name);
}

std::string_view package_display_name(const PackageManifest& manifest) {
    return manifest.project ? std::string_view(manifest.project->display_name)
                            : package_distribution_name(manifest);
}

std::optional<std::string> package_host_platform() {
#ifdef _WIN32
#  if defined(_M_X64) || defined(__x86_64__)
    return "windows-x86_64";
#  elif defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#  else
    return std::nullopt;
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "macos-arm64";
#  elif defined(__x86_64__)
    return "macos-x86_64";
#  else
    return std::nullopt;
#  endif
#elif defined(__linux__)
#  if defined(__x86_64__)
    return "linux-x86_64";
#  elif defined(__aarch64__)
    return "linux-arm64";
#  else
    return std::nullopt;
#  endif
#else
    return std::nullopt;
#endif
}

std::optional<fs::path> package_native_library_path(
    const fs::path& package_root,
    const PackageManifest& manifest) {
    const std::string* configured = nullptr;
    if (const auto platform = package_host_platform()) {
        if (const auto found = manifest.native_libraries.find(*platform);
            found != manifest.native_libraries.end()) {
            configured = &found->second;
        }
    }
    if (!configured) {
        if (const auto fallback = manifest.native_libraries.find("default");
            fallback != manifest.native_libraries.end()) {
            configured = &fallback->second;
        }
    }
    if (!configured) return std::nullopt;

    fs::path relative(*configured);
    if (relative.empty() || relative.is_absolute()) {
        throw std::runtime_error(
            "package native library path must be package-relative: " +
            *configured);
    }
    for (const auto& component : relative) {
        if (component == "..") {
            throw std::runtime_error(
                "package native library path may not escape the package root: " +
                *configured);
        }
    }

    const auto root = fs::absolute(package_root).lexically_normal();
    const auto resolved = fs::absolute(root / relative).lexically_normal();
    const auto inside = resolved.lexically_relative(root);
    if (inside.empty() || inside.is_absolute() ||
        (!inside.empty() && *inside.begin() == "..")) {
        throw std::runtime_error(
            "package native library path may not escape the package root: " +
            *configured);
    }

    std::error_code error;
    if (!fs::is_regular_file(resolved, error) || error) {
        throw std::runtime_error(
            "package native library is missing or is not a regular file: " +
            resolved.string());
    }
    return resolved;
}

std::vector<fs::path> package_native_source_paths(
    const fs::path& package_root,
    const PackageManifest& manifest) {
    const auto root = fs::absolute(package_root).lexically_normal();
    std::vector<fs::path> result;
    result.reserve(manifest.native_sources.size());
    const auto append_source = [&](const std::string& configured) {
        fs::path relative(configured);
        if (relative.empty() || relative.is_absolute()) {
            throw std::runtime_error(
                "package native source path must be package-relative: " + configured);
        }
        for (const auto& component : relative) {
            if (component == "..") {
                throw std::runtime_error(
                    "package native source path may not escape the package root: " +
                    configured);
            }
        }
        const auto resolved = fs::absolute(root / relative).lexically_normal();
        const auto inside = resolved.lexically_relative(root);
        if (inside.empty() || inside.is_absolute() ||
            (!inside.empty() && *inside.begin() == "..")) {
            throw std::runtime_error(
                "package native source path may not escape the package root: " +
                configured);
        }
        std::error_code error;
        if (!fs::is_regular_file(resolved, error) || error) {
            throw std::runtime_error(
                "package native source is missing or is not a regular file: " +
                resolved.string());
        }
        result.push_back(resolved);
    };
    for (const auto& [name, configured] : manifest.native_sources) {
        (void)name;
        append_source(configured);
    }
    if (const auto platform = package_host_platform()) {
        if (const auto found = manifest.native_platform_sources.find(*platform);
            found != manifest.native_platform_sources.end()) {
            for (const auto& [name, configured] : found->second) {
                (void)name;
                append_source(configured);
            }
        }
    }
    return result;
}

std::map<std::string, fs::path> package_compiler_extension_paths(
    const fs::path& package_root,
    const PackageManifest& manifest) {
    std::map<std::string, fs::path> result;
    if (!manifest.project) return result;

    const auto root = fs::absolute(package_root).lexically_normal();
    for (const auto& [name, configured] :
         manifest.project->compiler_extensions) {
        fs::path relative(configured);
        if (relative.empty() || relative.is_absolute()) {
            throw std::runtime_error(
                "package compiler extension path must be package-relative: " +
                configured);
        }
        for (const auto& component : relative) {
            if (component == "..") {
                throw std::runtime_error(
                    "package compiler extension path may not escape the package "
                    "root: " + configured);
            }
        }
        const auto resolved = fs::absolute(root / relative).lexically_normal();
        const auto inside = resolved.lexically_relative(root);
        if (inside.empty() || inside.is_absolute() ||
            (!inside.empty() && *inside.begin() == "..")) {
            throw std::runtime_error(
                "package compiler extension path may not escape the package "
                "root: " + configured);
        }
        std::error_code error;
        if (!fs::is_regular_file(resolved, error) || error) {
            throw std::runtime_error(
                "package compiler extension descriptor is missing or is not a "
                "regular file: " + resolved.string());
        }
        result.emplace(name, resolved);
    }
    return result;
}

std::string SemanticVersion::str() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch);
}

SemanticVersion parse_semantic_version(std::string_view raw) {
    const auto text = trim(raw);
    const auto first = text.find('.');
    const auto second =
        first == std::string::npos ? std::string::npos : text.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        text.find('.', second + 1) != std::string::npos) {
        throw std::runtime_error(
            "semantic version must have the form MAJOR.MINOR.PATCH");
    }
    return SemanticVersion{
        parse_component(std::string_view(text).substr(0, first)),
        parse_component(
            std::string_view(text).substr(first + 1, second - first - 1)),
        parse_component(std::string_view(text).substr(second + 1))};
}

VersionRequirement parse_version_requirement(std::string_view raw) {
    VersionRequirement requirement;
    requirement.text = trim(raw);
    if (requirement.text.empty()) {
        throw std::runtime_error("version requirement cannot be empty");
    }
    if (requirement.text == "*") return requirement;

    std::istringstream input(requirement.text);
    std::string token;
    while (input >> token) {
        VersionOperator op = VersionOperator::Equal;
        std::string_view version_text = token;
        if (version_text.starts_with(">=")) {
            op = VersionOperator::GreaterEqual;
            version_text.remove_prefix(2);
        } else if (version_text.starts_with("<=")) {
            op = VersionOperator::LessEqual;
            version_text.remove_prefix(2);
        } else if (version_text.starts_with(">")) {
            op = VersionOperator::Greater;
            version_text.remove_prefix(1);
        } else if (version_text.starts_with("<")) {
            op = VersionOperator::Less;
            version_text.remove_prefix(1);
        } else if (version_text.starts_with("=")) {
            op = VersionOperator::Equal;
            version_text.remove_prefix(1);
        }
        if (version_text.empty()) {
            throw std::runtime_error(
                "version requirement is missing a version after its operator");
        }
        requirement.clauses.push_back(
            VersionClause{op, parse_semantic_version(version_text)});
    }
    if (requirement.clauses.empty()) {
        throw std::runtime_error("version requirement cannot be empty");
    }
    return requirement;
}

bool VersionRequirement::matches(const SemanticVersion& candidate_version) const {
    for (const auto& clause : clauses) {
        if (!clause_matches(clause, candidate_version)) return false;
    }
    return true;
}

PackageManifest read_package_manifest(const fs::path& package_root) {
    const auto path =
        package_root / std::string(package_manifest_filename);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("package is missing quidra.package: " +
                                 path.string());
    }

    std::unordered_map<std::string, std::string> fields;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == '#') continue;
        const auto equal = cleaned.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error(
                "quidra.package line " + std::to_string(line_number) +
                " must contain '='");
        }
        const auto key = trim(std::string_view(cleaned).substr(0, equal));
        const auto value = trim(std::string_view(cleaned).substr(equal + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error(
                "quidra.package line " + std::to_string(line_number) +
                " has an empty key or value");
        }
        if (!fields.emplace(key, value).second) {
            throw std::runtime_error("duplicate quidra.package key: " + key);
        }
    }

    const auto name = fields.find("name");
    const auto version_field = fields.find("version");
    if (name == fields.end()) {
        throw std::runtime_error("quidra.package requires 'name'");
    }
    if (version_field == fields.end()) {
        throw std::runtime_error("quidra.package requires 'version'");
    }

    PackageManifest manifest;
    manifest.name = name->second;
    manifest.version = parse_semantic_version(version_field->second);
    if (const auto repository = fields.find("repository");
        repository != fields.end()) {
        manifest.repository = repository->second;
    }
    if (const auto description = fields.find("description");
        description != fields.end()) {
        manifest.description = description->second;
    }
    if (const auto license = fields.find("license");
        license != fields.end()) {
        manifest.license = license->second;
    }
    if (const auto homepage = fields.find("homepage");
        homepage != fields.end()) {
        manifest.homepage = homepage->second;
    }

    for (const auto& [key, value] : fields) {
        if (key == "name" || key == "version" || key == "repository" ||
            key == "description" || key == "license" || key == "homepage") continue;

        constexpr std::string_view asset_prefix = "asset.";
        if (std::string_view(key).starts_with(asset_prefix)) {
            if (key.size() == asset_prefix.size()) {
                throw std::runtime_error("quidra.package asset key is missing a platform");
            }
            const auto platform = key.substr(asset_prefix.size());
            for (const unsigned char c : platform) {
                if (!(std::isalnum(c) || c == '-' || c == '_')) {
                    throw std::runtime_error(
                        "quidra.package asset platform contains an invalid character: " +
                        platform);
                }
            }
            manifest.assets.emplace(platform, value);
            continue;
        }

        constexpr std::string_view native_source_prefix = "native.source.";
        if (std::string_view(key).starts_with(native_source_prefix)) {
            if (key.size() == native_source_prefix.size()) {
                throw std::runtime_error(
                    "quidra.package native source key is missing a name");
            }
            const auto source_key = key.substr(native_source_prefix.size());
            const auto separator = source_key.find('.');
            const auto platform = separator == std::string::npos
                ? std::string{} : source_key.substr(0, separator);
            const auto source_name = separator == std::string::npos
                ? source_key : source_key.substr(separator + 1);
            const auto validate_component = [&](const std::string& component,
                                                const char* label) {
                if (component.empty())
                    throw std::runtime_error(
                        std::string("quidra.package native source ") + label +
                        " is empty");
                for (const unsigned char ch : component)
                    if (!(std::isalnum(ch) || ch == '-' || ch == '_'))
                        throw std::runtime_error(
                            std::string("quidra.package native source ") + label +
                            " contains an invalid character: " + component);
            };
            if (!platform.empty()) validate_component(platform, "platform");
            validate_component(source_name, "name");
            fs::path source_path(value);
            if (source_path.empty() || source_path.is_absolute()) {
                throw std::runtime_error(
                    "quidra.package native source path must be package-relative: " +
                    value);
            }
            for (const auto& component : source_path) {
                if (component == "..") {
                    throw std::runtime_error(
                        "quidra.package native source path may not escape the package root: " +
                        value);
                }
            }
            if (platform.empty())
                manifest.native_sources.emplace(source_name, value);
            else
                manifest.native_platform_sources[platform].emplace(source_name, value);
            continue;
        }

        constexpr std::string_view native_pkg_prefix = "native.pkg.";
        if (std::string_view(key).starts_with(native_pkg_prefix)) {
            if (key.size() == native_pkg_prefix.size()) {
                throw std::runtime_error(
                    "quidra.package native pkg key is missing a name");
            }
            const auto dependency_name = key.substr(native_pkg_prefix.size());
            for (const unsigned char ch : dependency_name) {
                if (!(std::isalnum(ch) || ch == '-' || ch == '_')) {
                    throw std::runtime_error(
                        "quidra.package native pkg name contains an invalid character: " +
                        dependency_name);
                }
            }
            if (value.empty()) {
                throw std::runtime_error(
                    "quidra.package native pkg module must not be empty");
            }
            for (const unsigned char ch : value) {
                if (!(std::isalnum(ch) || ch == '-' || ch == '_' ||
                      ch == '.' || ch == '+')) {
                    throw std::runtime_error(
                        "quidra.package native pkg module contains an invalid character: " +
                        value);
                }
            }
            manifest.native_pkg_config.emplace(dependency_name, value);
            continue;
        }

        constexpr std::string_view native_prefix = "native.";
        if (std::string_view(key).starts_with(native_prefix)) {
            if (key.size() == native_prefix.size()) {
                throw std::runtime_error(
                    "quidra.package native key is missing a platform");
            }
            const auto platform = key.substr(native_prefix.size());
            for (const unsigned char c : platform) {
                if (!(std::isalnum(c) || c == '-' || c == '_')) {
                    throw std::runtime_error(
                        "quidra.package native platform contains an invalid character: " +
                        platform);
                }
            }
            fs::path native_path(value);
            if (native_path.empty() || native_path.is_absolute()) {
                throw std::runtime_error(
                    "quidra.package native path must be package-relative: " +
                    value);
            }
            for (const auto& component : native_path) {
                if (component == "..") {
                    throw std::runtime_error(
                        "quidra.package native path may not escape the package root: " +
                        value);
                }
            }
            manifest.native_libraries.emplace(platform, value);
            continue;
        }

        constexpr std::string_view prefix = "requires.";
        if (!std::string_view(key).starts_with(prefix) ||
            key.size() == prefix.size()) {
            throw std::runtime_error("unknown quidra.package key: " + key);
        }
        const auto dependency = key.substr(prefix.size());
        manifest.requirements.emplace(
            dependency, parse_version_requirement(value));
    }
    manifest.project = read_package_project(package_root, manifest);
    return manifest;
}

std::optional<PackageManifest> try_read_package_manifest(
    const fs::path& package_root) {
    std::error_code error;
    const auto path =
        package_root / std::string(package_manifest_filename);
    if (!fs::exists(path, error) && !error) return std::nullopt;
    if (error || !fs::is_regular_file(path, error) || error) {
        throw std::runtime_error(
            "quidra.package exists but is not a regular file");
    }
    return read_package_manifest(package_root);
}

} // namespace quidra
