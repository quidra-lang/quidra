#pragma once

// Which environment variables can change what a cached run builds, and how
// the run cache's key covers each of them. Every variable that Core reads,
// and every variable the native toolchain reads that changes its output, has
// one row; docs/development.md shows the same classes in its "Run cache key"
// column, and tests/metadata_ssot_tests.py (check_cache_key_environment)
// requires that the column and this table agree, so no variable can be
// added to either without being classified.
//
//   value   the key holds the variable's value (unset, empty and every value
//           differ): the compiler driver, toolchain roots, the runtime
//           library, and the variables the toolchain's child processes read
//   effect  the variable only steers a search (PATH, HOME,
//           QUIDRA_PACKAGE_PATH); the key holds what the search found, and a
//           cached run repeats the search, so another value that finds the
//           same files still hits
//   no      the variable never changes what is built: it is read by the
//           program at run time, by the REPL, the debugger or the package
//           commands, or it only says where the cache is
//
// The toolchain's variables are keyed on every platform, also those only one
// platform's tools read; setting one elsewhere costs at most a rebuild.

#include "platform/environment.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::toolchain {

enum class CacheKeyClass { value, effect, no };

struct CompileEnvironmentVariable {
    std::string_view name;
    CacheKeyClass key;
};

inline constexpr std::array compile_environment{
    // Package resolution and the user's directories.
    CompileEnvironmentVariable{"QUIDRA_PACKAGE_PATH", CacheKeyClass::effect},
    CompileEnvironmentVariable{"HOME", CacheKeyClass::effect},
    CompileEnvironmentVariable{"USERPROFILE", CacheKeyClass::effect},
    CompileEnvironmentVariable{"PATH", CacheKeyClass::effect},
    CompileEnvironmentVariable{"QUIDRA_CACHE_DIR", CacheKeyClass::no},
    CompileEnvironmentVariable{"XDG_CACHE_HOME", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_ALLOW_FILE_PACKAGE_ASSETS", CacheKeyClass::no},
    // The native toolchain and Quidra's own files.
    CompileEnvironmentVariable{"QUIDRA_CLANGXX", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_CLANG", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_NVCC", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_CUDA_HOME", CacheKeyClass::value},
    CompileEnvironmentVariable{"CUDA_HOME", CacheKeyClass::value},
    CompileEnvironmentVariable{"CUDA_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_RUNTIME_LIBRARY", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_NATIVE_INCLUDE_DIR", CacheKeyClass::value},
    CompileEnvironmentVariable{"QUIDRA_PKG_CONFIG", CacheKeyClass::value},
    // Read only by `quidra build --lib`, which is never cached.
    CompileEnvironmentVariable{"QUIDRA_AR", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_LIBTOOL", CacheKeyClass::no},
    // The REPL, its JIT and the debugger.
    CompileEnvironmentVariable{"QUIDRA_LLI", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_LLVM_LIBRARY", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_ORC_RUNTIME", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_COMPILER_RT_BUILTINS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_JIT_RUNTIME_LIBRARY", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_DEBUGGER", CacheKeyClass::no},
    // Read by the program when it runs.
    CompileEnvironmentVariable{"QUIDRA_CPU_THREADS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_ERROR_FORMAT", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_BROADCAST", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_SAVED_TENSORS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_AUTOGRAD_PRUNE", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_AUTOGRAD_STATS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_COUNTERS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_COUNTERS_STEP", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_UNIFIED_MEMORY", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_UNIFIED_MEMORY_STATS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_GPU_ZERO_FILL", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_STREAM", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_MAX_OPS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_MAX_BYTES", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_IDLE_OPS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_IDLE_NS", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_POOL", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_POOL_BYTES", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_FAST_MATH", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_SAFE_MATH", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_UPLOAD", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_METAL_HOST_FILL_MAX", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_POOL_POISON", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_METAL_FAIL_COMMAND", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_FAKE_GPU_COUNT", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_FAKE_GPU_SYNC_FAIL", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX", CacheKeyClass::no},
    CompileEnvironmentVariable{"QUIDRA_TEST_FAKE_GPU_UNIFIED", CacheKeyClass::no},
    // Read by the native toolchain's child processes (clang, the linker,
    // pkg-config, nvcc), not by Core.
    CompileEnvironmentVariable{"CPATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"C_INCLUDE_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"CPLUS_INCLUDE_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"OBJC_INCLUDE_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"OBJCPLUS_INCLUDE_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"LIBRARY_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"COMPILER_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"CCC_OVERRIDE_OPTIONS", CacheKeyClass::value},
    CompileEnvironmentVariable{"PKG_CONFIG_PATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"PKG_CONFIG_LIBDIR", CacheKeyClass::value},
    CompileEnvironmentVariable{"PKG_CONFIG_SYSROOT_DIR", CacheKeyClass::value},
    CompileEnvironmentVariable{"PKG_CONFIG_ALLOW_SYSTEM_CFLAGS", CacheKeyClass::value},
    CompileEnvironmentVariable{"PKG_CONFIG_ALLOW_SYSTEM_LIBS", CacheKeyClass::value},
    CompileEnvironmentVariable{"NVCC_PREPEND_FLAGS", CacheKeyClass::value},
    CompileEnvironmentVariable{"NVCC_APPEND_FLAGS", CacheKeyClass::value},
    CompileEnvironmentVariable{"SDKROOT", CacheKeyClass::value},
    CompileEnvironmentVariable{"DEVELOPER_DIR", CacheKeyClass::value},
    CompileEnvironmentVariable{"MACOSX_DEPLOYMENT_TARGET", CacheKeyClass::value},
    CompileEnvironmentVariable{"ZERO_AR_DATE", CacheKeyClass::value},
    CompileEnvironmentVariable{"INCLUDE", CacheKeyClass::value},
    CompileEnvironmentVariable{"LIB", CacheKeyClass::value},
    CompileEnvironmentVariable{"LIBPATH", CacheKeyClass::value},
    CompileEnvironmentVariable{"VCINSTALLDIR", CacheKeyClass::value},
    CompileEnvironmentVariable{"VCToolsInstallDir", CacheKeyClass::value},
    CompileEnvironmentVariable{"WindowsSdkDir", CacheKeyClass::value},
    CompileEnvironmentVariable{"WindowsSDKVersion", CacheKeyClass::value},
    CompileEnvironmentVariable{"UniversalCRTSdkDir", CacheKeyClass::value},
    CompileEnvironmentVariable{"UCRTVersion", CacheKeyClass::value},
};

// The variables of class `value` with their current values (nullopt when
// unset), in table order: the environment part of a run cache key.
inline std::vector<std::pair<std::string_view, std::optional<std::string>>>
compile_environment_values() {
    std::vector<std::pair<std::string_view, std::optional<std::string>>> values;
    for (const auto& variable : compile_environment) {
        if (variable.key != CacheKeyClass::value) continue;
        values.emplace_back(
            variable.name, platform::environment_value(std::string(variable.name).c_str()));
    }
    return values;
}

} // namespace quidra::toolchain
