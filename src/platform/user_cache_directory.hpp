#pragma once

// Where Quidra keeps the user's caches: the platform's per-user cache
// directory, never a project directory.
//
//   QUIDRA_CACHE_DIR set and not empty   that directory (made absolute against
//                                         the working directory), everywhere
//   macOS                                 $HOME/Library/Caches/Quidra
//                                         (XDG_CACHE_HOME is not a macOS
//                                         convention and is ignored)
//   Linux and other POSIX systems         $XDG_CACHE_HOME/quidra when
//                                         XDG_CACHE_HOME is absolute (the XDG
//                                         specification ignores a relative
//                                         value), else $HOME/.cache/quidra
//   Windows                               <Local AppData>\Quidra\Cache, the
//                                         known folder (SHGetKnownFolderPath,
//                                         which follows folder redirection)
//
// HOME must be an absolute path; when it is not (unset, empty or relative),
// or the Windows known folder cannot be resolved, there is no cache
// directory. Callers then work without a cache.
//
// The directory is only named here; private_directory.hpp creates and
// checks it.

#include <filesystem>
#include <optional>
#include <string>

namespace quidra::platform {

// The conventions above, so that each can be checked on any host.
enum class CacheDirectoryConvention { macos, xdg, windows };

// The convention of the host this was compiled for.
CacheDirectoryConvention host_cache_directory_convention();

// What the conventions read: environment values (nullopt when unset), the
// resolved Windows known folder and the working directory.
struct CacheDirectoryInputs {
    std::optional<std::string> quidra_cache_dir;
    std::optional<std::string> home;
    std::optional<std::string> xdg_cache_home;
    std::optional<std::filesystem::path> local_app_data;
    std::filesystem::path working_directory;
};

std::optional<std::filesystem::path> user_cache_directory(
    CacheDirectoryConvention convention, const CacheDirectoryInputs& inputs);

// The cache directory of this process: the host convention over this
// process's environment and working directory.
std::optional<std::filesystem::path> user_cache_directory();

} // namespace quidra::platform
