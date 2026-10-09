#pragma once

// The keys of the run cache: what decides whether a cached executable may
// run in place of a new build of the same program.
//
// A cached run is found in two steps.
//
// The pre-key is the SHA-256 of what is known before anything is built
// (RunCachePreKeyMaterial): the compiler (version, ABI and IR versions, and
// its build id, compiler_identity.hpp), the host platform and the OS
// version, the C++ driver as discovery names and resolves it, the runtime
// library and the native extension include directory as the installation
// layout resolves them, the entry source's path, the command's working
// directory, the --link inputs as given, the compile and link options, and
// the values of the environment variables of class `value`
// (compile_environment.hpp). An index file per pre-key lists the keys of
// the entries recently built with it.
//
// The key is the SHA-256 of the pre-key material and of everything the
// build recorded: the toolchain snapshot (toolchain_identity.hpp), the
// digest of the native link recipe's template, and every dependency record
// in the order it was recorded:
//   source, manifest, project, descriptor, lock
//       a file the compiler read: path, size and SHA-256 of its content
//   absent         a path the compiler looked at that held no regular file,
//                  and what it held then (missing or other)
//   local_import   a local import's resolution: importer, import text, path
//   package        an installed package's resolution: name, main module
//   package_tree   the tree digest of a locked package (package_tree_sha256)
//   native_source, native_library, link_input, runtime_library
//       a file the native build used: path, size and SHA-256
//   native_header  a header a native compile read, by content
//   system_file    a system or SDK header, or a file the linker opened, by
//                  identity
// A cached entry is used only when every record still holds and its key,
// recomputed from what its metadata says, is the name it is stored under.
//
// KeyDocument serializes the material canonically: every field is preceded
// by its tag, and every value by its type and its length, so that two
// different materials never produce the same bytes. The first bytes name
// the schema (run_cache_key_schema); a schema change makes every older
// entry unreachable.

#include "platform/file_identity.hpp"
#include "platform/sha256.hpp"
#include "toolchain/toolchain_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::toolchain {

inline constexpr std::string_view run_cache_key_schema = "quidra-run-key/1";

// One compile or link option that can change a successful build's output,
// by name ("compile.debug_info") and value as text ("false").
struct RunCacheOption {
    std::string name;
    std::string value;
    friend bool operator==(const RunCacheOption&, const RunCacheOption&) = default;
};

// A --link input: the text the command was given and the absolute path it
// names.
struct RunCacheLinkInput {
    std::string given;
    std::filesystem::path path;
    friend bool operator==(const RunCacheLinkInput&, const RunCacheLinkInput&) = default;
};

struct RunCachePreKeyMaterial {
    std::string compiler_version;
    std::int64_t abi_version{};
    std::string ir_version;
    std::string build_id;
    std::string platform;
    std::string os_version;
    // The driver as discovery names it, and the program that name resolves to.
    std::string driver_requested;
    std::filesystem::path driver_path;
    std::filesystem::path runtime_library;
    std::optional<std::filesystem::path> native_include_directory;
    // The entry source as the compiler names it (absolute, lexically
    // normal, symbolic links kept), and the command's working directory.
    std::filesystem::path entry;
    std::filesystem::path working_directory;
    std::vector<RunCacheLinkInput> link_inputs;
    std::vector<RunCacheOption> options;
    // The variables of class `value`, in table order (nullopt: unset).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    friend bool operator==(const RunCachePreKeyMaterial&, const RunCachePreKeyMaterial&) = default;
};

enum class RunCacheDependencyKind {
    source,
    manifest,
    project,
    descriptor,
    lock,
    absent,
    local_import,
    package,
    package_tree,
    native_source,
    native_library,
    link_input,
    runtime_library,
    native_header,
    system_file,
};

// One record; which fields a kind uses is listed above (package and
// package_tree keep the package's main module in `path`).
struct RunCacheDependency {
    RunCacheDependencyKind kind{RunCacheDependencyKind::source};
    std::filesystem::path path;
    std::uint64_t size{};
    std::string sha256;
    // absent: "missing" or "other".
    std::string state;
    // local_import.
    std::filesystem::path importer;
    std::string target;
    // package.
    std::string name;
    // system_file.
    platform::FileIdentity identity;
    friend bool operator==(const RunCacheDependency&, const RunCacheDependency&) = default;
};

// Whether a kind is recorded with path, size and SHA-256.
bool run_cache_content_kind(RunCacheDependencyKind kind);
const char* run_cache_dependency_kind_name(RunCacheDependencyKind kind);
std::optional<RunCacheDependencyKind> run_cache_dependency_kind(std::string_view name);

class KeyDocument {
public:
    explicit KeyDocument(std::string_view schema);

    // A field's tag; the values that follow belong to it.
    void field(std::string_view tag);
    void text(std::string_view value);
    void integer(std::int64_t value);
    void unsigned_integer(std::uint64_t value);
    void boolean(bool value);
    void path(const std::filesystem::path& value);
    // An absent value differs from every present one, the empty text
    // included.
    void optional_text(const std::optional<std::string>& value);
    void optional_path(const std::optional<std::filesystem::path>& value);
    void identity(const platform::FileIdentity& value);
    // The number of elements of a list that follows.
    void count(std::size_t value);

    // The SHA-256 of the document, as 64 lowercase hex digits. The object is
    // not used afterwards.
    std::string finish();

private:
    void value(char type, std::string_view bytes);

    platform::Sha256 hash_;
};

std::string run_cache_pre_key(const RunCachePreKeyMaterial& material);

std::string run_cache_key(
    const RunCachePreKeyMaterial& material, const ToolchainSnapshot& toolchain,
    std::string_view recipe_sha256, const std::vector<RunCacheDependency>& dependencies);

// Whether `text` is 64 lowercase hex digits, the form of every key and
// digest the cache stores.
bool run_cache_digest_text(std::string_view text);

} // namespace quidra::toolchain
