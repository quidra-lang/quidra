#pragma once

// The native toolchain and host a cached run's executable depends on, taken
// from what the tools say about themselves, and checked again on a hit.
//
// snapshot_toolchain() probes the toolchain of a native link recipe whose
// inputs exist (the recipe has run, or every input it names is in place):
//   - the C++ driver as discovery names it, where that name resolves, the
//     driver's --version text, and the directory it lives in;
//   - `driver -###` over the link's own arguments: the compiler the driver
//     really runs (through xcrun shims and ccache: the program of its
//     compile job), the configuration files it read, the resource
//     directory, the target triple, CPU and features, and on Apple
//     platforms the SDK (-isysroot, -target-sdk-version, the digest of its
//     SDKSettings.json) and the target of /var/db/xcode_select_link;
//   - the linker (the program of the link job) and its -v text, and its
//     library and framework search lists (ld64 -v; for other linkers the -L
//     directories of the link job and the linker's built-in SEARCH_DIRs);
//   - for every native compile, the include search lists (`driver <flags>
//     -E -v`, once per distinct flag set) and the directories the driver
//     adds for system headers;
//   - with pkg-config modules, the pkg-config program, its --version text
//     and its --cflags and --libs output.
// A snapshot that cannot describe the toolchain completely says so
// (complete is false, with the reason): a driver that does not resolve, a
// probe without the expected output, CUDA sources (their header search is
// not recorded), plain assembly sources (their .include files are not
// recorded), a Windows link (it has no trace yet). Nothing may then be
// cached.
//
// toolchain_dependencies() turns what a run-mode recipe recorded into
// records: each header a native compile read (by content when it is the
// package's or the user's, by identity when it lies in a directory the
// driver adds for the system or the SDK), each file the linker opened (by
// identity), and the shadowing paths (driver_output.hpp) that must stay
// absent. Files the caller records by content (the runtime library, the
// package natives, the --link inputs, the LLVM file) and the recipe's own
// objects are left out.
//
// toolchain_unchanged() and dependency_unchanged() are the checks of a hit:
// discovery is repeated and every identity, digest and absence compared;
// the probes are not run again, except pkg-config, whose output is the
// input itself.
//
// The identities include each file's change time. A caller that builds and
// then snapshots must not keep a snapshot whose files changed while it
// built (the run cache's racy-input rule).

#include "platform/file_identity.hpp"
#include "toolchain/driver_output.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace quidra::toolchain {

class NativeLinkRecipe;

struct IdentifiedFile {
    std::filesystem::path path;
    platform::FileIdentity identity;
    friend bool operator==(const IdentifiedFile&, const IdentifiedFile&) = default;
};

// A program the toolchain runs, with the text it prints about its version.
struct ToolIdentity {
    IdentifiedFile program;
    std::string version_sha256;
    std::string version_line;
    friend bool operator==(const ToolIdentity&, const ToolIdentity&) = default;
};

struct DriverIdentity {
    // The driver as discovery names it (QUIDRA_CLANGXX, QUIDRA_CLANG or the
    // PATH candidate), and the program that name resolves to.
    std::string requested;
    ToolIdentity tool;
    IdentifiedFile directory;
    // The compiler the driver runs, and the directory it lives in (a
    // configuration file added there changes it).
    IdentifiedFile compiler;
    IdentifiedFile compiler_directory;
    std::vector<IdentifiedFile> configuration_files;
    std::optional<IdentifiedFile> resource_directory;
};

struct TargetIdentity {
    std::string triple;
    std::string cpu;
    std::vector<std::string> features;
    std::optional<std::filesystem::path> sysroot;
    std::string sdk_version;
    std::optional<std::string> sdk_settings_sha256;
    // Apple platforms: the target of /var/db/xcode_select_link, or nullopt
    // when there is no such link.
    std::optional<std::filesystem::path> developer_directory;
};

struct ToolchainSnapshot {
    DriverIdentity driver;
    TargetIdentity target;
    std::optional<ToolIdentity> linker;
    std::optional<LibrarySearch> library_search;
    // One per native compile step of the recipe, in step order (steps with
    // the same flags share one probe).
    std::vector<IncludeSearch> include_searches;
    // Directories the driver adds on its own (system and SDK headers and
    // frameworks), the sysroot and the resource directory.
    std::vector<std::filesystem::path> system_directories;
    std::optional<ToolIdentity> pkg_config;
    std::vector<std::string> pkg_config_modules;
    std::vector<std::string> pkg_config_cflags;
    std::vector<std::string> pkg_config_libs;
    std::optional<ToolIdentity> nvcc;
    bool complete{true};
    std::string incomplete_reason;
};

struct ToolchainDependency {
    enum class Kind { header, system_file, absent };
    Kind kind{Kind::absent};
    std::filesystem::path path;
    // header: the SHA-256 of its content.
    std::string sha256;
    // system_file: its identity.
    platform::FileIdentity identity;
    friend bool operator==(const ToolchainDependency&, const ToolchainDependency&) = default;
};

ToolchainSnapshot snapshot_toolchain(
    const NativeLinkRecipe& recipe, const std::vector<std::string>& pkg_config_modules);

// `complete` becomes false (with a reason in `incomplete_reason`) when a
// recorded file cannot be described: a depfile or trace that is missing, or
// a file it names that no longer exists.
std::vector<ToolchainDependency> toolchain_dependencies(
    const ToolchainSnapshot& snapshot, const NativeLinkRecipe& recipe,
    const std::vector<std::filesystem::path>& recorded_by_content,
    bool& complete, std::string& incomplete_reason);

bool toolchain_unchanged(const ToolchainSnapshot& snapshot);
bool dependency_unchanged(const ToolchainDependency& dependency);

} // namespace quidra::toolchain
