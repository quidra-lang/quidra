#pragma once

// Where the LLVM tools and libraries that Quidra drives are found: clang++
// for native code, lli for the REPL's JIT and the arguments it needs on this
// platform (on macOS the ORC runtime and compiler-rt builtins next to the
// LLVM installation lli comes from), the debugger, and the shared LLVM
// library the in-process JIT loads.
//
// Each lookup takes its QUIDRA_* override first (an empty value counts as
// unset), then tries a fixed list of candidates in order, and throws
// std::runtime_error with a message naming the override when nothing is
// found.

#include <filesystem>
#include <string>
#include <vector>

namespace quidra::toolchain {

// QUIDRA_CLANGXX, QUIDRA_CLANG, then clang++ by version.
std::string clang_driver();
// QUIDRA_LLI, then the LLVM installations and lli by version.
std::string jit_driver();
// QUIDRA_DEBUGGER, then lldb and gdb.
std::string debugger_driver();

// The arguments `lli_driver` needs on this platform to run JITed code: on
// macOS, JITLink with the ORC runtime (QUIDRA_ORC_RUNTIME, then the LLVM
// installation of `lli_driver` and the Homebrew prefixes) and the
// compiler-rt builtins (QUIDRA_COMPILER_RT_BUILTINS, then the same roots),
// for the emulated TLS of JITed thread_local globals; none elsewhere.
std::vector<std::string> jit_platform_arguments(const std::string& lli_driver);

// The shared LLVM libraries the in-process JIT tries to load, in order:
// QUIDRA_LLVM_LIBRARY, then the platform's names and installations by
// version.
std::vector<std::filesystem::path> llvm_library_candidates();

} // namespace quidra::toolchain
