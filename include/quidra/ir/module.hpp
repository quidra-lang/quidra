#pragma once

// ClassLayout and Module: the typed IR of a whole program, and ir::dump, its
// text (`quidra ir`).

#include "quidra/compiler_extension.hpp"
#include "quidra/ir/function.hpp"
#include "quidra/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quidra::ir {

struct ClassLayout {
    std::string name;
    std::vector<std::string> field_names;
    std::vector<Type> fields;
    // ClassDecl::standard_library. Not part of the dump.
    bool standard_library{};
};
// A user file of the program (Program::user_sources). Its index in
// Module::user_sources identifies it; the root is 0. A source location is
// user code when its file is listed, package code otherwise. The program's
// source table carries these fields, so a runtime failure names the file as
// the user spelled it and shows its source line only while the file is
// unchanged.
struct UserSource {
    std::string absolute_path;
    // The root as the user spelled it; an import, the root's directory joined
    // with the import's path relative to the root's directory.
    std::string display_path;
    // SHA-256 of the file's bytes (64 lowercase hex digits); empty where the
    // source is not a file (the REPL, string input).
    std::string revision;
    std::uint64_t line_count{};
    std::uint64_t byte_size{};
};
struct Module {
    std::vector<ClassLayout> classes;
    std::vector<Function> functions;
    std::vector<CompilerExtensionRegistration> compiler_extensions;
    std::vector<UserSource> user_sources;
};

std::string dump(const Module& module);

} // namespace quidra::ir
