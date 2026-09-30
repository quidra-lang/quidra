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
    bool matches(const SemanticVersion& candidate_version) const;
};

// The names a package declares in project.toml. quidra.package's `name` is the
// import identifier, and it is welded to the install directory, the repository
// basename and the lockfile key, so it cannot also carry a distribution name or
// a human-facing title. Those live here.
struct PackageProject {
    std::string distribution_name;
    std::string import_name;
    std::string display_name;
    std::string repository;
    std::optional<unsigned long long> abi_requirement;
    // Logical compiler-extension name -> package-relative declarative descriptor.
    // Extensions describe package-owned optimization semantics; Core only owns
    // validation, discovery and the generic compiler integration substrate.
    std::map<std::string, std::string> compiler_extensions;
};

struct PackageManifest {
    std::string name;
    SemanticVersion version;
    std::optional<std::string> repository;
    std::optional<std::string> description;
    std::optional<std::string> license;
    std::optional<std::string> homepage;
    std::map<std::string, std::string> assets;
    // Platform -> package-relative shared library path. These are implementation
    // components owned by the package, not Core runtime libraries.
    std::map<std::string, std::string> native_libraries;
    // Named package-relative native source inputs compiled by the host native
    // toolchain during AOT/JIT linking. Implementation stays package-owned.
    std::map<std::string, std::string> native_sources;
    // Platform -> (logical source name -> package-relative source path).
    // Host selection is a package mechanism; Core does not interpret semantics.
    std::map<std::string, std::map<std::string, std::string>>
        native_platform_sources;
    // Logical dependency name -> pkg-config module. Core only resolves generic
    // native dependency metadata; domain/vendor semantics remain package-owned.
    std::map<std::string, std::string> native_pkg_config;
    std::map<std::string, VersionRequirement> requirements;
    std::optional<PackageProject> project;
};

SemanticVersion parse_semantic_version(std::string_view text);
VersionRequirement parse_version_requirement(std::string_view text);

bool is_distribution_package_name(std::string_view name);
std::string_view package_distribution_name(const PackageManifest& manifest);
std::string_view package_import_name(const PackageManifest& manifest);
std::string_view package_display_name(const PackageManifest& manifest);

// Canonical platform ids shared by package assets and package-owned native
// components (for example linux-x86_64, macos-arm64, windows-x86_64).
std::optional<std::string> package_host_platform();

// Resolve the native component selected for this host. The manifest path is
// always package-relative; the resolved file must remain inside package_root.
std::optional<std::filesystem::path> package_native_library_path(
    const std::filesystem::path& package_root,
    const PackageManifest& manifest);
std::vector<std::filesystem::path> package_native_source_paths(
    const std::filesystem::path& package_root,
    const PackageManifest& manifest);
std::map<std::string, std::filesystem::path> package_compiler_extension_paths(
    const std::filesystem::path& package_root,
    const PackageManifest& manifest);

PackageManifest read_package_manifest(const std::filesystem::path& package_root);
std::optional<PackageManifest> try_read_package_manifest(
    const std::filesystem::path& package_root);

} // namespace quidra
