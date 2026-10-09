#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace quidra {

class CompileInputs;

struct PackageLockEntry {
    std::string distribution_name;
    bool distribution_name_explicit{};
    std::string version;
    std::string sha256;
};

using PackageLockEntries = std::map<std::string, PackageLockEntry>;

std::filesystem::path package_lock_path(
    const std::filesystem::path& project_root);
std::string package_tree_sha256(
    const std::filesystem::path& package_main);
// With `inputs`, the lockfile's content (or its absence) is recorded.
std::optional<PackageLockEntries> read_package_lock(
    const std::filesystem::path& project_root, CompileInputs* inputs = nullptr);
std::string package_lock_text(
    const std::map<std::string, std::filesystem::path>& packages);

} // namespace quidra
