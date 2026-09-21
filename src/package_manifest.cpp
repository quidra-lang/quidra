#include "quidra/package_manifest.hpp"

#include "quidra/toml_subset.hpp"

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

bool clause_matches(const VersionClause& clause, const SemanticVersion& version) {
    const int order = compare(version, clause.version);
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
    const auto path = package_root / "project.toml";
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
    const auto version = parse_semantic_version(required("version"));
    if (compare(version, manifest.version) != 0) {
        throw std::runtime_error(
            "project.toml package.version " + version.str() +
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
        std::string_view version = token;
        if (version.starts_with(">=")) {
            op = VersionOperator::GreaterEqual;
            version.remove_prefix(2);
        } else if (version.starts_with("<=")) {
            op = VersionOperator::LessEqual;
            version.remove_prefix(2);
        } else if (version.starts_with(">")) {
            op = VersionOperator::Greater;
            version.remove_prefix(1);
        } else if (version.starts_with("<")) {
            op = VersionOperator::Less;
            version.remove_prefix(1);
        } else if (version.starts_with("=")) {
            op = VersionOperator::Equal;
            version.remove_prefix(1);
        }
        if (version.empty()) {
            throw std::runtime_error(
                "version requirement is missing a version after its operator");
        }
        requirement.clauses.push_back(
            VersionClause{op, parse_semantic_version(version)});
    }
    if (requirement.clauses.empty()) {
        throw std::runtime_error("version requirement cannot be empty");
    }
    return requirement;
}

bool VersionRequirement::matches(const SemanticVersion& version) const {
    for (const auto& clause : clauses) {
        if (!clause_matches(clause, version)) return false;
    }
    return true;
}

PackageManifest read_package_manifest(const fs::path& package_root) {
    const auto path = package_root / "quidra.package";
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
    const auto version = fields.find("version");
    if (name == fields.end()) {
        throw std::runtime_error("quidra.package requires 'name'");
    }
    if (version == fields.end()) {
        throw std::runtime_error("quidra.package requires 'version'");
    }

    PackageManifest manifest;
    manifest.name = name->second;
    manifest.version = parse_semantic_version(version->second);
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
    const auto path = package_root / "quidra.package";
    if (!fs::exists(path, error) && !error) return std::nullopt;
    if (error || !fs::is_regular_file(path, error) || error) {
        throw std::runtime_error(
            "quidra.package exists but is not a regular file");
    }
    return read_package_manifest(package_root);
}

} // namespace quidra
