#pragma once

// ir::lower: the typed IR of a checked program (src/lowering). The one IR
// entry point that needs the checker; the IR headers under quidra/ir/ do not
// include it.

#include "quidra/checker.hpp"
#include "quidra/ir/module.hpp"

#include <cstddef>

namespace quidra::ir {

// Internal switches that turn a narrowing predicate of the lowering off, so
// that tests can compare the outputs of a program with and without it. They
// are not part of the language and default to off.
struct LoweringOptions {
    // Every compound assignment to an element or a field resolves its target
    // again after the right-hand side, not only those whose right-hand side
    // may write the target's root storage.
    bool reresolve_every_compound_store{};
    // Every value collection loop over storage iterates a copy of the array
    // taken at loop entry, not only those whose body may write it.
    bool copy_every_value_loop_at_entry{};
    // In every reference loop over a reference binding, every statement of
    // the body is followed by the check of the iterated array, not only
    // those that may replace or resize it.
    bool check_every_reference_loop_statement{};
    // Every borrowed by-value argument that names storage is passed as a
    // copy, not only those the call may write while it runs.
    bool copy_every_borrowed_argument{};
    // Every plain assignment evaluates its target's subexpressions before
    // its right-hand side, not only those whose order can be observed.
    bool reorder_every_assignment{};
};

// A Library artifact has no entry function: its root holds declarations
// only, and its C host has its own main.
Module lower(
    const CheckedProgram& checked,
    const Expr* repl_expression = nullptr,
    std::size_t replay_prefix_bytes = 0,
    LoweringOptions options = {},
    CompileArtifact artifact = CompileArtifact::Executable);

} // namespace quidra::ir
