#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace quidra {

using PackageLockEntries = std::map<std::string, std::string>;

std::filesystem::path package_lock_path(const std::filesystem::path& project_root);
std::string package_tree_sha256(const std::filesystem::path& package_main);
std::optional<PackageLockEntries> read_package_lock(
    const std::filesystem::path& project_root);
std::string package_lock_text(
    const std::map<std::string, std::filesystem::path>& packages);

} // namespace quidra
