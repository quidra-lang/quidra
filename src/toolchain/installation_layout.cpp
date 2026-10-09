#include "toolchain/installation_layout.hpp"

#include "platform/environment.hpp"
#include "platform/executable.hpp"

#include <array>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace quidra::toolchain {

fs::path runtime_library() {
    if (const auto configured = platform::environment_value("QUIDRA_RUNTIME_LIBRARY");
        configured && !configured->empty()) {
        fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error("QUIDRA_RUNTIME_LIBRARY does not name a file: " + path.string());
    }

    const auto bin = platform::executable_path().parent_path();
#ifdef _WIN32
    const char* library_name = "quidra_runtime.lib";
#else
    const char* library_name = "libquidra_runtime.a";
#endif

    const std::array<fs::path, 5> candidates{{
        bin / library_name,
        (bin / "../lib/quidra" / library_name).lexically_normal(),
        (bin / "../lib64/quidra" / library_name).lexically_normal(),
        (bin / "../lib/x86_64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib/aarch64-linux-gnu/quidra" / library_name).lexically_normal(),
    }};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }

    throw std::runtime_error(
        "cannot locate the Quidra runtime library; set QUIDRA_RUNTIME_LIBRARY explicitly");
}

fs::path jit_runtime_library() {
    if (const auto configured = platform::environment_value("QUIDRA_JIT_RUNTIME_LIBRARY");
        configured && !configured->empty()) {
        fs::path path = *configured;
        if (fs::is_regular_file(path)) return path;
        throw std::runtime_error(
            "QUIDRA_JIT_RUNTIME_LIBRARY does not name a file: " + path.string());
    }

    const auto bin = platform::executable_path().parent_path();
#ifdef _WIN32
    const char* library_name = "quidra_runtime_jit.dll";
#elif defined(__APPLE__)
    const char* library_name = "libquidra_runtime_jit.dylib";
#else
    const char* library_name = "libquidra_runtime_jit.so";
#endif

    const std::array<fs::path, 6> candidates{{
        bin / library_name,
        (bin / "../lib/quidra" / library_name).lexically_normal(),
        (bin / "../lib64/quidra" / library_name).lexically_normal(),
        (bin / "../lib/x86_64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib/aarch64-linux-gnu/quidra" / library_name).lexically_normal(),
        (bin / "../lib" / library_name).lexically_normal(),
    }};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }

    throw std::runtime_error(
        "cannot locate the Quidra JIT runtime library; set QUIDRA_JIT_RUNTIME_LIBRARY explicitly");
}

std::optional<fs::path> native_extension_include_directory() {
    if (const auto configured = platform::environment_value("QUIDRA_NATIVE_INCLUDE_DIR");
        configured && !configured->empty()) {
        fs::path root = fs::absolute(*configured).lexically_normal();
        if (fs::is_regular_file(root / "quidra" / "native_extension.h"))
            return root;
        throw std::runtime_error(
            "QUIDRA_NATIVE_INCLUDE_DIR must contain quidra/native_extension.h: " +
            root.string());
    }

    const auto bin = platform::executable_path().parent_path();
    const std::array<fs::path, 3> candidates{{
        (bin / "../include").lexically_normal(),
        (bin / "../../include").lexically_normal(),
        (bin / "include").lexically_normal(),
    }};
    for (const auto& root : candidates) {
        std::error_code error;
        if (fs::is_regular_file(
                root / "quidra" / "native_extension.h", error) && !error)
            return fs::absolute(root).lexically_normal();
    }
    return std::nullopt;
}

} // namespace quidra::toolchain
