#pragma once

// The runtime prelude: the LLVM text that follows the header line of every
// generated module. It declares the runtime entry points and C library
// functions that generated code calls, defines the thread-local source
// position and call-depth globals, and defines the helpers written in LLVM IR
// itself (quidra_fail_at, quidra_stack_enter, quidra_array_slot, the checked
// integer arithmetic, ...).
//
// The declarations of the runtime entry points are written from their
// signatures (runtime_abi.hpp); the rest of the text is written as it is. The
// whole text is written once, at compile time. It is part of the output byte
// for byte; tests/llvm_backend_tests.cpp checks it.

#include "llvm_text/llvm_callee.hpp"

#include <span>
#include <string_view>

namespace quidra::llvm_backend {

std::string_view runtime_prelude_text();

// The runtime entry points the prelude declares, in its order
// (runtime_abi.hpp).
std::span<const llvm_text::LlvmCallee* const> runtime_prelude_declarations();

} // namespace quidra::llvm_backend
