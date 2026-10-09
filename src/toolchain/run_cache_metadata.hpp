#pragma once

// The files of a run cache entry that describe it, and the checks of a hit.
//
// metadata.json holds, in readable form, everything the entry's key is made
// of (run_cache_key.hpp), the key and the pre-key, and the program and
// toolchain output it stores:
//
//   { "schema": 1, "kind": "quidra-run-cache-entry", "key": ..., "pre_key": ...,
//     "compiler": { "version", "abi", "ir", "build_id" },
//     "host": { "platform", "os" },
//     "invocation": { "entry", "cwd", "driver": { "requested", "path" },
//                     "runtime_library", "native_include_directory",
//                     "link_inputs": [ { "given", "path" } ],
//                     "options": [ { "name", "value" } ], "recipe_sha256" },
//     "environment": [ { "name", "value" } ],
//     "toolchain": { "driver", "target", "linker", "library_search",
//                    "include_searches", "system_directories", "pkg_config",
//                    "pkg_config_modules", "pkg_config_cflags",
//                    "pkg_config_libs", "nvcc" },
//     "dependencies": [ { "kind", ... } ],
//     "program": { "file", "size", "sha256" },
//     "toolchain_output": { "file", "size", "sha256" },
//     "created_unix_ns", "build_wall_ms" }
//
// File identities are written as objects of decimal strings (a device or
// inode number need not fit a JSON integer). Only complete toolchain
// snapshots are ever stored.
//
// Reading is strict: a document that is not exactly of this shape (an
// unknown or missing field, an unknown dependency kind, a higher schema, a
// digest that is not 64 lowercase hex digits, a negative size) is corrupt,
// and read_run_cache_metadata throws RunCacheCorruptError. The caller then
// discards the entry.
//
// index/<pre-key>.json lists the keys of the entries recently built with
// one pre-key, newest first, at most run_cache_index_limit of them:
//   { "schema": 1, "kind": "quidra-run-cache-index", "pre_key": ..., "keys": [...] }
//
// recheck_dependency() checks the records whose check needs no compiler:
// a file read by content is read again and must have the same size and
// SHA-256; an absent path must hold what it held (still nothing, or still
// no regular file); a system file must have the same identity. The
// resolutions and package trees (local_import, package, package_tree) are
// the caller's to check.

#include "toolchain/run_cache_key.hpp"
#include "toolchain/toolchain_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::toolchain {

inline constexpr std::int64_t run_cache_metadata_schema = 1;
inline constexpr std::size_t run_cache_index_limit = 4;

class RunCacheCorruptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A file the entry stores, by its name in the entry directory.
struct RunCacheStoredFile {
    std::string file;
    std::uint64_t size{};
    std::string sha256;
    friend bool operator==(const RunCacheStoredFile&, const RunCacheStoredFile&) = default;
};

struct RunCacheEntry {
    std::string key;
    std::string pre_key;
    RunCachePreKeyMaterial material;
    ToolchainSnapshot toolchain;
    std::string recipe_sha256;
    std::vector<RunCacheDependency> dependencies;
    RunCacheStoredFile program;
    RunCacheStoredFile toolchain_output;
    std::int64_t created_unix_ns{};
    std::int64_t build_wall_ms{};
};

std::string write_run_cache_metadata(const RunCacheEntry& entry);
RunCacheEntry read_run_cache_metadata(std::string_view text);

struct RunCacheIndex {
    std::string pre_key;
    std::vector<std::string> keys;
};

std::string write_run_cache_index(const RunCacheIndex& index);
// nullopt when the text is not a valid index.
std::optional<RunCacheIndex> read_run_cache_index(std::string_view text);
// `index` with `key` first, without repeats, cut to the limit.
RunCacheIndex run_cache_index_with(RunCacheIndex index, const std::string& key);

enum class RunCacheRecheck { unchanged, changed, not_checked_here };

RunCacheRecheck recheck_dependency(const RunCacheDependency& dependency);

// The size and SHA-256 of a file's whole content, read in binary mode;
// nullopt when it cannot be read.
struct RunCacheContentDigest {
    std::uint64_t size{};
    std::string sha256;
};
std::optional<RunCacheContentDigest> run_cache_file_digest(const std::filesystem::path& path);

} // namespace quidra::toolchain
