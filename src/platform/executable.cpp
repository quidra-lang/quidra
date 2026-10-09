#include "platform/executable.hpp"

#include "platform/environment.hpp"

#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include "platform/native_text.hpp"
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <vector>
#else
#include <array>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::platform {

fs::path executable_path() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("cannot locate the Quidra executable");
    }
    buffer.resize(length);
    return fs::path(buffer);
#elif defined(__APPLE__)
    std::uint32_t size = 4096;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            throw std::runtime_error("cannot locate the Quidra executable");
        }
    }
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(fs::path(buffer.data()), ec);
    return ec ? fs::path(buffer.data()) : canonical;
#else
    std::array<char, 4096> buffer{};
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count <= 0) throw std::runtime_error("cannot locate the Quidra executable");
    return fs::path(std::string(buffer.data(), static_cast<std::size_t>(count)));
#endif
}

std::optional<fs::path> command_path(std::string_view candidate) {
#ifdef _WIN32
    const auto wide = native_text(candidate);
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(
        nullptr, wide.c_str(), nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    buffer.resize(length);
    return fs::path(buffer);
#else
    const fs::path requested(candidate);
    if (requested.has_parent_path()) {
        if (::access(requested.c_str(), X_OK) != 0) return std::nullopt;
        std::error_code error;
        const auto canonical = fs::weakly_canonical(requested, error);
        return error ? fs::absolute(requested).lexically_normal() : canonical;
    }
    const auto path_variable = platform::environment_value("PATH");
    if (!path_variable) return std::nullopt;
    std::string_view path(*path_variable);
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        const auto part = path.substr(
            start, end == std::string_view::npos ? path.size() - start : end - start);
        const fs::path directory =
            part.empty() ? fs::path(".") : fs::path(std::string(part));
        const auto executable = directory / requested;
        if (::access(executable.c_str(), X_OK) == 0) {
            std::error_code error;
            const auto canonical = fs::weakly_canonical(executable, error);
            return error ? fs::absolute(executable).lexically_normal() : canonical;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return std::nullopt;
#endif
}

bool command_available(const char* candidate) {
    return command_path(candidate).has_value();
}

} // namespace quidra::platform
