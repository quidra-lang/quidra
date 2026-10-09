#pragma once

// The identity of the Quidra compiler that builds a cached run: its
// version, ABI version and IR version, and its build id, the SHA-256 of the
// compiler executable itself, so that two builds of the same version (every
// development build) never share a cached program.
//
// The toolchain layer does not see the project header, so the caller
// passes the versions it was built with.
//
// Hashing the executable on every run would cost more than the rest of a
// cache hit, so the digest is remembered in a memo,
// <cache root>/compiler-id/<SHA-256 of the executable's real path>.json, a
// private file that holds the executable's file identity and digest. The
// memo is used only while the executable's identity is unchanged. It is
// written only when the executable last changed more than 2 seconds before
// the hashing started: a file rewritten within the clock's resolution could
// otherwise keep the identity the memo describes while its content is new.
// A file that changes while it is hashed has no build id.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace quidra::toolchain {

struct CompilerVersions {
    std::string version;
    int abi{};
    std::string ir;
    friend bool operator==(const CompilerVersions&, const CompilerVersions&) = default;
};

struct CompilerIdentity {
    CompilerVersions versions;
    std::string build_id;
};

// The SHA-256 of `executable` (its real path), through the memos in
// `memo_directory` when one is given. `hashing_start` is the time the racy
// guard compares the executable's change time with.
std::optional<std::string> compiler_build_id(
    const std::filesystem::path& executable,
    const std::optional<std::filesystem::path>& memo_directory,
    std::chrono::system_clock::time_point hashing_start = std::chrono::system_clock::now());

// The running compiler: `versions` and the build id of this executable,
// with the memos under `cache_root`/compiler-id when a cache root is given.
// nullopt when the executable cannot be located or read.
std::optional<CompilerIdentity> compiler_identity(
    const CompilerVersions& versions, const std::optional<std::filesystem::path>& cache_root);

} // namespace quidra::toolchain
