#pragma once

// ForeignExportEmitter: the C entry point of an exported function
// (export "C").
//
// The entry point is a definition under the function's C symbol, its bare
// declared name, with the C signature of its fixed-width scalar parameters
// and result (foreign_abi.hpp: the result carries the callee's extension of
// 8- and 16-bit integers, the parameters none). It registers the module's
// source table, so that a failure inside the function names its file and
// line, calls the function's own symbol (callee_symbol: the user symbol, the
// depth wrapper of a self-depth recursive function), and, when the function
// may leave device work behind (ir/analysis/device_reach.hpp), calls
// quidra_runtime_export_leave before it returns to C. Quidra callers keep
// calling the function's own symbol, so the entry point costs them nothing;
// LLVM inlines the body into it.
//
// Owns no state. The module writes the entry points after every function
// body, in the module's order, so a module without exports is unchanged.

#include "quidra/ir/module.hpp"

#include <string>

namespace quidra::llvm_backend {

std::string emit_foreign_export(const ir::Function& function, const std::string& callee_symbol,
                                bool device_reaching);

} // namespace quidra::llvm_backend
