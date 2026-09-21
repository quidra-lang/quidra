#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {

struct SemanticVersion {
    unsigned long long major{};
    unsigned long long minor{};
    unsigned long long patch{};
    std::string str() const;
};

enum class VersionOperator { Equal, Less, LessEqual, Greater, GreaterEqual };

struct VersionClause {
    VersionOperator op{VersionOperator::Equal};
    SemanticVersion version;
};

struct VersionRequirement {
    std::string text;
    std::vector<VersionClause> clauses;
    bool matches(const SemanticVersion& version) const;
};

// The names a package declares in project.toml. quidra.package's `name` is the
// import identifier, and it is welded to the install directory, the repository
// basename and the lockfile key, so it cannot also carry a distribution name or
// a human-facing title. Those live here.
struct PackageProject {
    std::string distribution_name;
    std::string import_name;
    std::string display_name;
    std::optional<unsigned long long> abi_requirement;
};

struct PackageManifest {
    std::string name;
    SemanticVersion version;
    std::optional<std::string> repository;
    std::optional<std::string> description;
    std::optional<std::string> license;
    std::optional<std::string> homepage;
    std::map<std::string, std::string> assets;
    std::map<std::string, VersionRequirement> requirements;
    std::optional<PackageProject> project;
};

SemanticVersion parse_semantic_version(std::string_view text);
VersionRequirement parse_version_requirement(std::string_view text);
PackageManifest read_package_manifest(const std::filesystem::path& package_root);
std::optional<PackageManifest> try_read_package_manifest(
    const std::filesystem::path& package_root);

} // namespace quidra
