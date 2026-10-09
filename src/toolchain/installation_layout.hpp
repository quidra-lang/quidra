#pragma once

// Where an installed (or built) Quidra keeps its own files, relative to the
// running executable: the runtime library programs link, the JIT runtime the
// REPL loads, and the native extension header packages compile against.
// Each lookup takes its QUIDRA_* override first.

#include <filesystem>
#include <optional>

namespace quidra::toolchain {

// QUIDRA_RUNTIME_LIBRARY, then the runtime archive next to the executable
// or in the installation's lib directories; throws when there is none.
std::filesystem::path runtime_library();
// QUIDRA_JIT_RUNTIME_LIBRARY, then the JIT runtime the same way.
std::filesystem::path jit_runtime_library();
// QUIDRA_NATIVE_INCLUDE_DIR (which must contain quidra/native_extension.h),
// then the include directories around the executable, or nullopt.
std::optional<std::filesystem::path> native_extension_include_directory();

} // namespace quidra::toolchain
