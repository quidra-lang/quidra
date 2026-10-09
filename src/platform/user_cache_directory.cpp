#include "platform/user_cache_directory.hpp"

#include "platform/environment.hpp"

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace quidra::platform {
namespace {

std::optional<fs::path> absolute_directory(const std::optional<std::string>& value) {
    if (!value || value->empty()) return std::nullopt;
    const fs::path path(*value);
    if (!path.is_absolute()) return std::nullopt;
    return path.lexically_normal();
}

#ifdef _WIN32
std::optional<fs::path> known_local_app_data() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    std::optional<fs::path> folder;
    if (SUCCEEDED(result) && raw && *raw) folder = fs::path(raw);
    CoTaskMemFree(raw);
    return folder;
}
#endif

} // namespace

CacheDirectoryConvention host_cache_directory_convention() {
#if defined(_WIN32)
    return CacheDirectoryConvention::windows;
#elif defined(__APPLE__)
    return CacheDirectoryConvention::macos;
#else
    return CacheDirectoryConvention::xdg;
#endif
}

std::optional<fs::path> user_cache_directory(
    CacheDirectoryConvention convention, const CacheDirectoryInputs& inputs) {
    if (inputs.quidra_cache_dir && !inputs.quidra_cache_dir->empty()) {
        const fs::path configured(*inputs.quidra_cache_dir);
        return configured.is_absolute()
            ? configured.lexically_normal()
            : (inputs.working_directory / configured).lexically_normal();
    }
    switch (convention) {
        case CacheDirectoryConvention::macos:
            if (const auto home = absolute_directory(inputs.home)) {
                return *home / "Library" / "Caches" / "Quidra";
            }
            return std::nullopt;
        case CacheDirectoryConvention::xdg:
            if (const auto xdg = absolute_directory(inputs.xdg_cache_home)) {
                return *xdg / "quidra";
            }
            if (const auto home = absolute_directory(inputs.home)) {
                return *home / ".cache" / "quidra";
            }
            return std::nullopt;
        case CacheDirectoryConvention::windows:
            if (inputs.local_app_data && inputs.local_app_data->is_absolute()) {
                return inputs.local_app_data->lexically_normal() / "Quidra" / "Cache";
            }
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<fs::path> user_cache_directory() {
    CacheDirectoryInputs inputs;
    inputs.quidra_cache_dir = environment_value("QUIDRA_CACHE_DIR");
    std::error_code error;
    inputs.working_directory = fs::current_path(error);
    if (error && inputs.quidra_cache_dir && !inputs.quidra_cache_dir->empty() &&
        !fs::path(*inputs.quidra_cache_dir).is_absolute()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    inputs.local_app_data = known_local_app_data();
#else
    inputs.home = environment_value("HOME");
#if !defined(__APPLE__)
    inputs.xdg_cache_home = environment_value("XDG_CACHE_HOME");
#endif
#endif
    return user_cache_directory(host_cache_directory_convention(), inputs);
}

} // namespace quidra::platform
