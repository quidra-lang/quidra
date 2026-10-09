#pragma once

// The diagnostic constants of every module: the printf formats of REPL and
// runtime output (@.fmt.*, @.bool.*), and the failure codes and messages
// (@.code.*, @.msg.*, @.err.*) that fail-fast guards and error results refer
// to by name. They are the first constants of a module, before the string
// pool.

#include "llvm_text/llvm_module.hpp"

namespace quidra::llvm_backend {

void emit_diagnostic_constants(llvm_text::LlvmModule& module);

} // namespace quidra::llvm_backend
