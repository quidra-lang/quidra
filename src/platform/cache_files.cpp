#include "platform/cache_files.hpp"

#include "platform/sha256.hpp"

#include <array>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::platform {

#ifdef _WIN32

bool sync_directory_files(const fs::path& directory) {
    std::error_code error;
    bool ok = true;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const HANDLE handle = CreateFileW(
            it->path().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            ok = false;
            continue;
        }
        ok = FlushFileBuffers(handle) != FALSE && ok;
        CloseHandle(handle);
    }
    return ok && !error;
}

std::error_code rename_directory(const fs::path& from, const fs::path& to) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return {};
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

std::optional<PrivateFileDigest> private_file_digest(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return std::nullopt;
    }
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    Sha256 hash;
    std::array<char, 1 << 16> buffer{};
    std::uint64_t size = 0;
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        hash.update(buffer.data(), read);
        size += read;
    }
    CloseHandle(handle);
    if (!ok) return std::nullopt;
    return PrivateFileDigest{size, hash.finish_hex()};
}

std::optional<std::uint64_t> allocated_size(const fs::path& path) {
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return std::nullopt;
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}

bool touch_file(const fs::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const bool ok = SetFileTime(handle, nullptr, nullptr, &now) != FALSE;
    CloseHandle(handle);
    return ok;
}

bool link_or_copy_file(const fs::path& from, const fs::path& to) {
    if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) return true;
    return CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE;
}

#else

namespace {

bool sync_path(const fs::path& path, int flags) {
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

} // namespace

bool sync_directory_files(const fs::path& directory) {
    std::error_code error;
    bool ok = true;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code status_error;
        if (!it->is_regular_file(status_error) || it->is_symlink(status_error)) continue;
        ok = sync_path(it->path(), O_RDONLY | O_NOFOLLOW) && ok;
    }
    return !error && ok && sync_path(directory, O_RDONLY | O_DIRECTORY);
}

std::error_code rename_directory(const fs::path& from, const fs::path& to) {
    if (::rename(from.c_str(), to.c_str()) == 0) return {};
    return std::error_code(errno, std::generic_category());
}

std::optional<PrivateFileDigest> private_file_digest(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid()) {
        ::close(fd);
        return std::nullopt;
    }
    Sha256 hash;
    std::array<char, 1 << 16> buffer{};
    std::uint64_t size = 0;
    bool ok = true;
    for (;;) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (count == 0) break;
        hash.update(buffer.data(), static_cast<std::size_t>(count));
        size += static_cast<std::uint64_t>(count);
    }
    ::close(fd);
    if (!ok) return std::nullopt;
    return PrivateFileDigest{size, hash.finish_hex()};
}

std::optional<std::uint64_t> allocated_size(const fs::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) return std::nullopt;
    return static_cast<std::uint64_t>(status.st_blocks) * 512U;
}

bool touch_file(const fs::path& path) {
    return ::utimes(path.c_str(), nullptr) == 0;
}

bool link_or_copy_file(const fs::path& from, const fs::path& to) {
    if (::link(from.c_str(), to.c_str()) == 0) return true;
    if (errno == EEXIST) return false;
    std::error_code error;
    if (!fs::copy_file(from, to, fs::copy_options::none, error) || error) {
        fs::remove(to, error);
        return false;
    }
    if (::chmod(to.c_str(), 0700) != 0) {
        fs::remove(to, error);
        return false;
    }
    return true;
}

#endif

} // namespace quidra::platform
