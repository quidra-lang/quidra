#include "toolchain/llvm_discovery.hpp"

#include "platform/environment.hpp"
#include "platform/executable.hpp"

#include <stdexcept>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace quidra::toolchain {

std::string clang_driver() {
    if (const auto configured = platform::environment_value("QUIDRA_CLANGXX");
        configured && !configured->empty()) {
        return *configured;
    }
    if (const auto configured = platform::environment_value("QUIDRA_CLANG");
        configured && !configured->empty()) {
        return *configured;
    }
#ifdef _WIN32
    for (const char* candidate :
         {"clang++.exe", "clang++-20.exe", "clang++-19.exe", "clang++-18.exe", "clang++-17.exe"}) {
#else
    for (const char* candidate :
         {"clang++-20", "clang++-19", "clang++-18", "clang++-17", "clang++-16", "clang++-15", "clang++"}) {
#endif
        if (platform::command_available(candidate)) return candidate;
    }
    throw std::runtime_error("Clang++ 15 or newer is required for native code generation");
}

std::string jit_driver() {
    if (const auto configured = platform::environment_value("QUIDRA_LLI");
        configured && !configured->empty()) {
        if (!platform::command_available(configured->c_str()) && !fs::is_regular_file(fs::path(*configured))) {
            throw std::runtime_error(
                "QUIDRA_LLI is not executable or not on PATH: " + *configured);
        }
        return *configured;
    }
#ifdef _WIN32
    const fs::path installed = "C:/Program Files/LLVM/bin/lli.exe";
    if (fs::is_regular_file(installed)) return installed.string();
    for (const char* candidate :
         {"lli.exe", "lli-20.exe", "lli-19.exe", "lli-18.exe", "lli-17.exe", "lli-16.exe", "lli-15.exe"}) {
#elif defined(__APPLE__)
    for (const fs::path& installed : {
             fs::path("/opt/homebrew/opt/llvm/bin/lli"),
             fs::path("/usr/local/opt/llvm/bin/lli")}) {
        if (fs::is_regular_file(installed)) return installed.string();
    }
    for (const char* candidate :
         {"lli-20", "lli-19", "lli-18", "lli-17", "lli-16", "lli-15", "lli"}) {
#else
    for (const char* candidate :
         {"lli-20", "lli-19", "lli-18", "lli-17", "lli-16", "lli-15", "lli"}) {
#endif
        if (platform::command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "LLVM lli 15 or newer is required for JIT execution; install LLVM or set QUIDRA_LLI");
}

std::string debugger_driver() {
    if (const auto configured = platform::environment_value("QUIDRA_DEBUGGER");
        configured && !configured->empty()) {
        if (!platform::command_available(configured->c_str())) {
            throw std::runtime_error(
                "QUIDRA_DEBUGGER is not executable or not on PATH: " + *configured);
        }
        return *configured;
    }
#ifdef _WIN32
    for (const char* candidate : {"lldb.exe", "gdb.exe"}) {
#else
    for (const char* candidate : {"lldb", "gdb"}) {
#endif
        if (platform::command_available(candidate)) return candidate;
    }
    throw std::runtime_error(
        "no supported debugger found; install lldb/gdb or set QUIDRA_DEBUGGER");
}

#ifdef __APPLE__
namespace {

// The LLVM roots to search for runtime libraries: the installation of
// `lli_driver`, then the Homebrew prefixes.
std::vector<fs::path> llvm_runtime_roots(const std::string& lli_driver) {
    std::vector<fs::path> roots;
    const fs::path driver_path = lli_driver;
    if (driver_path.has_parent_path()) {
        const auto bin = driver_path.parent_path();
        if (bin.filename() == "bin") roots.push_back(bin.parent_path());
    }
    roots.emplace_back("/opt/homebrew/opt/llvm");
    roots.emplace_back("/usr/local/opt/llvm");
    return roots;
}

fs::path find_clang_runtime_library(
    const std::vector<fs::path>& roots,
    const std::vector<std::string_view>& names) {
    for (const auto& root : roots) {
        const auto clang_root = root / "lib" / "clang";
        std::error_code ec;
        if (!fs::is_directory(clang_root, ec) || ec) continue;
        for (fs::recursive_directory_iterator it(
                 clang_root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }
            const auto name = it->path().filename().string();
            for (const auto candidate : names) {
                if (name == candidate) return it->path();
            }
        }
    }
    return {};
}

// QUIDRA_ORC_RUNTIME, then the ORC runtime under the LLVM installation of
// `lli_driver` and the Homebrew prefixes.
fs::path orc_runtime_library(const std::string& lli_driver) {
    if (const auto configured = platform::environment_value("QUIDRA_ORC_RUNTIME");
        configured && !configured->empty()) {
        const fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_ORC_RUNTIME does not name a file: " + path.string());
    }

    const auto found = find_clang_runtime_library(
        llvm_runtime_roots(lli_driver),
        {"libclang_rt.orc_osx.a", "liborc_rt_osx.a", "liborc_rt.a"});
    if (!found.empty()) return found;

    throw std::runtime_error(
        "LLVM ORC runtime for macOS was not found; install Homebrew llvm or set "
        "QUIDRA_ORC_RUNTIME to libclang_rt.orc_osx.a");
}

// QUIDRA_COMPILER_RT_BUILTINS, then libclang_rt.osx.a under the same roots.
fs::path compiler_rt_builtins_library(const std::string& lli_driver) {
    if (const auto configured = platform::environment_value("QUIDRA_COMPILER_RT_BUILTINS");
        configured && !configured->empty()) {
        const fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_COMPILER_RT_BUILTINS does not name a file: " + path.string());
    }

    const auto found = find_clang_runtime_library(
        llvm_runtime_roots(lli_driver), {"libclang_rt.osx.a"});
    if (!found.empty()) return found;

    throw std::runtime_error(
        "compiler-rt builtins for macOS were not found; install Homebrew llvm or set "
        "QUIDRA_COMPILER_RT_BUILTINS to libclang_rt.osx.a");
}

} // namespace
#endif

std::vector<std::string> jit_platform_arguments([[maybe_unused]] const std::string& lli_driver) {
    std::vector<std::string> arguments;
#ifdef __APPLE__
    // ORC lowers JITed thread_local globals to emulated TLS on Darwin.
    // Supplying compiler-rt's ORC runtime provides the TLS entry point and
    // lifecycle instead of relying on process-symbol lookup.
    arguments.emplace_back("--jit-linker=jitlink");
    arguments.emplace_back("--orc-runtime=" + orc_runtime_library(lli_driver).string());
    // JITed TLS on Darwin is lowered through compiler-rt's emulated TLS ABI.
    // The ORC platform runtime does not provide __emutls_get_address itself,
    // so make the ordinary compiler-rt builtins archive available to JITLink.
    arguments.emplace_back(
        "--extra-archive=" + compiler_rt_builtins_library(lli_driver).string());
#endif
    return arguments;
}

std::vector<fs::path> llvm_library_candidates() {
    std::vector<fs::path> result;
    if (const auto configured = platform::environment_value("QUIDRA_LLVM_LIBRARY");
        configured && !configured->empty()) {
        result.emplace_back(*configured);
    }
#ifdef _WIN32
    result.emplace_back("LLVM-C.dll");
    result.emplace_back("LLVM.dll");
    result.emplace_back(R"(C:\Program Files\LLVM\bin\LLVM-C.dll)");
    result.emplace_back(R"(C:\Program Files\LLVM\bin\LLVM.dll)");
#elif defined(__APPLE__)
    result.emplace_back("libLLVM.dylib");
    result.emplace_back("/opt/homebrew/opt/llvm/lib/libLLVM.dylib");
    result.emplace_back("/usr/local/opt/llvm/lib/libLLVM.dylib");
    for (int llvm_major = 24; llvm_major >= 15; --llvm_major) {
        result.emplace_back("/opt/homebrew/opt/llvm@" + std::to_string(llvm_major) + "/lib/libLLVM.dylib");
        result.emplace_back("/usr/local/opt/llvm@" + std::to_string(llvm_major) + "/lib/libLLVM.dylib");
    }
#else
    result.emplace_back("libLLVM.so");
    for (int llvm_major = 24; llvm_major >= 15; --llvm_major) {
        result.emplace_back("libLLVM-" + std::to_string(llvm_major) + ".so");
        result.emplace_back("libLLVM-" + std::to_string(llvm_major) + ".so.1");
        result.emplace_back("/usr/lib/llvm-" + std::to_string(llvm_major) + "/lib/libLLVM.so");
        result.emplace_back("/usr/lib/llvm-" + std::to_string(llvm_major) + "/lib/libLLVM.so.1");
    }
#endif
    return result;
}

} // namespace quidra::toolchain
