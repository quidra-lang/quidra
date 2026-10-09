#pragma once

// Reading the process environment: the one place in Core that does it.
//
// The compiler and CLI (toolchain overrides, the package store), the
// generated-program runtime (switches, test hooks, the language's environment
// builtins) and the device backends all read environment variables through
// these two functions. docs/development.md ("Environment variables") lists
// every variable Core reads, with its default and the values it accepts.
//
// The functions only read. What an unset, empty or malformed value means is
// the caller's policy and stays at the call site.
//
// Windows reads through _dupenv_s: the CRT deprecates std::getenv there
// (C4996, an error under /WX). The header has no source file, so every target
// that reads the environment (compiler, runtime archive, JIT runtime,
// WebAssembly frontend) compiles it with its own flags and needs no extra
// source in its list.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace quidra::platform {

// The value of environment variable `name`, or nullopt when it is not set. A
// variable set to the empty string yields an empty value.
inline std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::nullopt;
    // `size` counts the terminating NUL.
    std::string value(raw, size > 0 ? size - 1 : 0);
    std::free(raw);
    return value;
#else
    if (const char* raw = std::getenv(name)) return std::string(raw);
    return std::nullopt;
#endif
}

// Whether environment variable `name` is set to exactly `expected`. On POSIX
// this makes no copy of the value, so a runtime failure path may ask it.
inline bool environment_value_is(const char* name, const char* expected) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    const bool equal = _dupenv_s(&raw, &size, name) == 0 && raw != nullptr &&
                       std::strcmp(raw, expected) == 0;
    std::free(raw);
    return equal;
#else
    const char* raw = std::getenv(name);
    return raw != nullptr && std::strcmp(raw, expected) == 0;
#endif
}

// Whether environment variable `name` is set, possibly to the empty string.
// On POSIX this makes no copy of the value.
inline bool environment_has(const char* name) {
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    const bool present = _dupenv_s(&raw, &size, name) == 0 && raw != nullptr;
    std::free(raw);
    return present;
#else
    return std::getenv(name) != nullptr;
#endif
}

} // namespace quidra::platform
