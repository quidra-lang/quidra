#include "quidra/package_lock.hpp"
#include "quidra/compile_inputs.hpp"
#include "quidra/package_manifest.hpp"
#include "quidra/project.hpp"

#include "platform/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {
namespace {

namespace fs = std::filesystem;

// A length, as the package tree hash writes it: 8 bytes, big-endian.
void update_u64(platform::Sha256& hash, std::uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (int i = 7; i >= 0; --i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(value & 0xffU);
        value >>= 8U;
    }
    hash.update(bytes.data(), bytes.size());
}

std::string portable_relative_path(const fs::path& path, const fs::path& root) {
    const auto relative = fs::relative(path, root).lexically_normal();
#if defined(__cpp_char8_t)
    const auto raw = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
#else
    return relative.generic_u8string();
#endif
}

bool valid_lock_name(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
    });
}

} // namespace

fs::path package_lock_path(const fs::path& project_root) {
    return fs::absolute(project_root).lexically_normal() /
           std::string(package_lock_filename);
}

std::string package_tree_sha256(const fs::path& package_main) {
    const auto main = fs::absolute(package_main).lexically_normal();
    std::error_code error;
    if (!fs::is_regular_file(main, error) || error ||
        main.filename() != package_entrypoint_filename()) {
        throw std::runtime_error(
            "package hash requires package entrypoint " +
            package_entrypoint_filename());
    }
    const auto root = main.parent_path();

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
        const auto status = iterator->symlink_status(error);
        if (error) throw std::runtime_error("cannot inspect package tree: " + error.message());
        if (fs::is_directory(status) && iterator->path().filename() == ".git") {
            iterator.disable_recursion_pending();
            continue;
        }
        if (fs::is_symlink(status)) {
            throw std::runtime_error(
                "package hashing rejects symbolic links: " + iterator->path().string());
        }
        if (fs::is_regular_file(status)) files.push_back(iterator->path());
        else if (!fs::is_directory(status)) {
            throw std::runtime_error(
                "package hashing encountered unsupported filesystem entry: " +
                iterator->path().string());
        }
    }
    std::sort(files.begin(), files.end(), [&](const fs::path& left, const fs::path& right) {
        return portable_relative_path(left, root) < portable_relative_path(right, root);
    });

    platform::Sha256 hash;
    hash.update("quidra-package-tree-v1");
    for (const auto& file : files) {
        const auto relative = portable_relative_path(file, root);
        update_u64(hash, static_cast<std::uint64_t>(relative.size()));
        hash.update(relative);

        const auto size = fs::file_size(file, error);
        if (error) throw std::runtime_error("cannot stat package file: " + file.string());
        update_u64(hash, static_cast<std::uint64_t>(size));

        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read package file: " + file.string());
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
        if (!input.eof()) throw std::runtime_error("cannot finish reading package file: " + file.string());
    }
    return hash.finish_hex();
}

std::optional<PackageLockEntries> read_package_lock(
    const fs::path& project_root, CompileInputs* inputs) {
    const auto path = package_lock_path(project_root);
    const auto state = probe_input_path(path, inputs);
    if (state == InputPathState::missing) return std::nullopt;
    if (state != InputPathState::regular_file) {
        throw std::runtime_error("quidra.lock exists but is not a regular file");
    }

    const auto text = read_input_file(path, InputFileKind::lock, inputs);
    if (!text) throw std::runtime_error("cannot read quidra.lock");
    std::istringstream input(*text);

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("quidra.lock is empty");
    }
    const bool legacy_v1 = line == "quidra-lock-v1";
    const bool legacy_v2 = line == "quidra-lock-v2";
    const bool current = line == current_lockfile_header();
    if (!legacy_v1 && !legacy_v2 && !current) {
        throw std::runtime_error(
            "quidra.lock must begin with a supported quidra-lock header");
    }

    PackageLockEntries entries;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;

        std::istringstream fields(line);
        std::string distribution_name;
        std::string name;
        std::string package_version = "-";
        std::string digest;
        std::string extra;
        if (legacy_v1) {
            if (!(fields >> name >> digest) || (fields >> extra)) {
                throw std::runtime_error(
                    "invalid quidra.lock entry on line " +
                    std::to_string(line_number));
            }
            distribution_name = name;
        } else if (legacy_v2) {
            if (!(fields >> name >> package_version >> digest) || (fields >> extra)) {
                throw std::runtime_error(
                    "invalid quidra.lock entry on line " +
                    std::to_string(line_number));
            }
            distribution_name = name;
        } else {
            if (!(fields >> distribution_name >> name >> package_version >> digest) ||
                (fields >> extra)) {
                throw std::runtime_error(
                    "invalid quidra.lock entry on line " +
                    std::to_string(line_number));
            }
        }

        if (!valid_lock_name(distribution_name) || !valid_lock_name(name) ||
            !valid_sha256(digest)) {
            throw std::runtime_error(
                "invalid quidra.lock entry on line " +
                std::to_string(line_number));
        }
        if (package_version != "-") {
            try {
                (void)parse_semantic_version(package_version);
            } catch (const std::exception&) {
                throw std::runtime_error(
                    "invalid package version in quidra.lock on line " +
                    std::to_string(line_number));
            }
        }

        if (!entries
                 .emplace(
                     name,
                     PackageLockEntry{
                         std::move(distribution_name),
                         !legacy_v1 && !legacy_v2,
                         std::move(package_version), std::move(digest)})
                 .second) {
            throw std::runtime_error(
                "duplicate package in quidra.lock: " + name);
        }
    }
    return entries;
}

std::string package_lock_text(
    const std::map<std::string, fs::path>& packages) {
    std::ostringstream output;
    output << current_lockfile_header() << '\n';

    for (const auto& [name, main] : packages) {
        if (!valid_lock_name(name)) {
            throw std::runtime_error(
                "invalid package name in resolved dependency set");
        }

        std::string distribution_name = name;
        std::string package_version = "-";
        if (const auto manifest =
                try_read_package_manifest(main.parent_path())) {
            package_version = manifest->version.str();
            distribution_name =
                std::string(package_distribution_name(*manifest));
            if (package_import_name(*manifest) != name) {
                throw std::runtime_error(
                    "resolved import name does not match package metadata");
            }
        }
        if (!valid_lock_name(distribution_name)) {
            throw std::runtime_error(
                "invalid distribution name in resolved dependency set");
        }

        output
            << distribution_name << ' '
            << name << ' '
            << package_version << ' '
            << package_tree_sha256(main) << '\n';
    }

    return output.str();
}

} // namespace quidra
