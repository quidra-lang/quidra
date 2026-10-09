#pragma once

// user_source_table: the program's user files as the source table carries
// them (ir::Module::user_sources): the root first, then the local imports in
// load order, each with its absolute path, display path, revision, line
// count and byte size.
//
// The root's display path is the program's (Program::source_display_path);
// an import's is the root's display directory joined with the import's path
// relative to the root's directory, lexically normalized, so it keeps the
// ".." steps that remain. A display path in angle brackets (<repl>,
// <memory>) names no file: its sources record an empty revision.

#include "quidra/ast.hpp"
#include "quidra/ir/module.hpp"

#include <vector>

namespace quidra::lowering {

std::vector<ir::UserSource> user_source_table(const Program& program);

} // namespace quidra::lowering
