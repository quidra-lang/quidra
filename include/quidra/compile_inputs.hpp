#pragma once

// CompileInputs: everything a compilation read from the file system, recorded
// while it reads, so that a later process can tell, without compiling,
// whether the same compilation would read the same things.
//
// The module loader, the package manifest and lock readers and the package
// resolution fill it when they are given one (a null pointer records nothing;
// string compilation, the REPL, the language server and WebAssembly pass
// none). Its records, in the order they were made:
//   - every file read, with the SHA-256 of the bytes read: module sources,
//     package manifests (quidra.package), project.toml files, compiler
//     extension descriptors and quidra.lock;
//   - every path that was looked at and held no regular file: a package
//     root probed before the one that holds the package, a missing
//     quidra.package, project.toml or quidra.lock;
//   - every local import resolution (importer, import text, result) and
//     every installed-package resolution (name, main module);
//   - the tree digest of every locked package (package_tree_sha256).
// A record that repeats an earlier one exactly is kept once.
//
// A read the records cannot describe (a file that exists but cannot be
// opened, so that the compilation went on without it) makes the inputs
// incomplete; nothing may then rely on them.
//
// The functions after the class are the only way the compiler reaches the
// file system for its inputs (tests/metadata_ssot_tests.py,
// check_compile_input_reads). They behave like the plain reads they replace
// whether or not a recorder is given.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace quidra {

enum class InputFileKind { source, manifest, project, descriptor, lock };

// What a path holds, following symbolic links: nothing (it does not exist,
// or a component of it is not a directory), a regular file, or anything
// else, including a path whose status cannot be read.
enum class InputPathState { missing, regular_file, other };

struct InputFile {
    InputFileKind kind{InputFileKind::source};
    std::filesystem::path path;
    std::uint64_t size{};
    std::string sha256;
    friend bool operator==(const InputFile&, const InputFile&) = default;
};

// A path that held no regular file: `state` is missing or other.
struct AbsentInput {
    std::filesystem::path path;
    InputPathState state{InputPathState::missing};
    friend bool operator==(const AbsentInput&, const AbsentInput&) = default;
};

struct LocalImportInput {
    std::filesystem::path importer;
    std::string target;
    std::filesystem::path path;
    friend bool operator==(const LocalImportInput&, const LocalImportInput&) = default;
};

struct PackageInput {
    std::string name;
    std::filesystem::path main;
    friend bool operator==(const PackageInput&, const PackageInput&) = default;
};

struct PackageTreeInput {
    std::filesystem::path main;
    std::string sha256;
    friend bool operator==(const PackageTreeInput&, const PackageTreeInput&) = default;
};

using CompileInput =
    std::variant<InputFile, AbsentInput, LocalImportInput, PackageInput, PackageTreeInput>;

class CompileInputs {
public:
    void record(CompileInput input);
    // Records that something was read in a way the records cannot describe;
    // the first reason is kept.
    void mark_incomplete(std::string reason);

    const std::vector<CompileInput>& records() const { return records_; }
    bool complete() const { return incomplete_reason_.empty(); }
    const std::string& incomplete_reason() const { return incomplete_reason_; }

private:
    std::vector<CompileInput> records_;
    std::string incomplete_reason_;
};

// The record kinds' names, as text views and metadata spell them.
const char* input_file_kind_name(InputFileKind kind);
const char* input_path_state_name(InputPathState state);

// What `path` holds now. Records nothing.
InputPathState input_path_state(const std::filesystem::path& path);

// input_path_state, and an AbsentInput record when `path` holds no regular
// file.
InputPathState probe_input_path(const std::filesystem::path& path, CompileInputs* inputs);

// The whole content of the file `path`, read as std::ifstream reads it in
// binary mode, recorded as an InputFile of `kind`. nullopt when the file
// cannot be opened; that is recorded as an AbsentInput when it holds no
// regular file and makes the inputs incomplete otherwise.
std::optional<std::string> read_input_file(
    const std::filesystem::path& path, InputFileKind kind, CompileInputs* inputs);

} // namespace quidra
