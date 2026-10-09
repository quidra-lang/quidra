#include "platform/file_lock.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::platform {

struct LockedFile::Native {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped{};
#else
    int fd{-1};
#endif
};

LockedFile::LockedFile(const fs::path& path, LockOpenMode mode, bool try_only)
    : native_(std::make_unique<Native>()) {
#ifdef _WIN32
    auto& handle = native_->handle;
    DWORD disposition = OPEN_EXISTING;
    if (mode == LockOpenMode::open_always) disposition = OPEN_ALWAYS;
    else if (mode == LockOpenMode::create_new) disposition = CREATE_NEW;
    handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (mode == LockOpenMode::open_existing && error == ERROR_FILE_NOT_FOUND) return;
        if (mode == LockOpenMode::create_new &&
            (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
            throw LockFileExistsError();
        }
        throw std::runtime_error(
            "cannot open Quidra run lock (Windows error " + std::to_string(error) + ")");
    }
    present_ = true;
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
    if (try_only) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(handle, flags, 0, 1, 0, &native_->overlapped)) {
        const DWORD error = GetLastError();
        if (try_only && error == ERROR_LOCK_VIOLATION) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            return;
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        throw std::runtime_error(
            "cannot lock Quidra run lock (Windows error " + std::to_string(error) + ")");
    }
    locked_ = true;
#else
    auto& fd = native_->fd;
    int flags = O_RDWR;
    if (mode == LockOpenMode::open_always) flags |= O_CREAT;
    else if (mode == LockOpenMode::create_new) flags |= O_CREAT | O_EXCL;
    fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) {
        if (mode == LockOpenMode::open_existing && errno == ENOENT) return;
        if (mode == LockOpenMode::create_new && errno == EEXIST) throw LockFileExistsError();
        throw std::runtime_error(
            "cannot open Quidra run lock: " + std::string(std::strerror(errno)));
    }
    present_ = true;
    const int operation = LOCK_EX | (try_only ? LOCK_NB : 0);
    if (::flock(fd, operation) != 0) {
        const int error = errno;
        if (try_only && (error == EWOULDBLOCK || error == EAGAIN)) {
            ::close(fd);
            fd = -1;
            return;
        }
        ::close(fd);
        fd = -1;
        throw std::runtime_error(
            "cannot lock Quidra run lock: " + std::string(std::strerror(error)));
    }
    locked_ = true;
#endif
}

LockedFile::~LockedFile() { close(); }

void LockedFile::write_all(std::string_view text) {
    if (!locked_) throw std::runtime_error("Quidra run lock is not held");
#ifdef _WIN32
    const HANDLE handle = native_->handle;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        throw std::runtime_error("cannot reset Quidra run lease");
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(
                text.size() - offset, static_cast<std::size_t>(0xffffffffu)));
        DWORD written = 0;
        if (!WriteFile(
                handle, text.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            throw std::runtime_error("cannot write Quidra run lease");
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        throw std::runtime_error("cannot flush Quidra run lease");
    }
#else
    const int fd = native_->fd;
    if (::ftruncate(fd, 0) != 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        throw std::runtime_error("cannot reset Quidra run lease");
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "cannot write Quidra run lease: " +
                std::string(std::strerror(errno)));
        }
        if (written == 0) throw std::runtime_error("cannot write Quidra run lease");
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(fd) != 0) throw std::runtime_error("cannot flush Quidra run lease");
#endif
}

std::string LockedFile::read_all() {
    if (!locked_) throw std::runtime_error("Quidra run lock is not held");
#ifdef _WIN32
    const HANDLE handle = native_->handle;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) ||
        size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        throw std::runtime_error("invalid Quidra run lease size");
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) {
        throw std::runtime_error("cannot read Quidra run lease");
    }
    std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(
                out.size() - offset, static_cast<std::size_t>(0xffffffffu)));
        if (!ReadFile(handle, out.data() + offset, chunk, &read, nullptr)) {
            throw std::runtime_error("cannot read Quidra run lease");
        }
        if (read == 0) break;
        offset += read;
    }
    out.resize(offset);
    return out;
#else
    const int fd = native_->fd;
    const auto end = ::lseek(fd, 0, SEEK_END);
    if (end < 0 || end > 1024 * 1024) {
        throw std::runtime_error("invalid Quidra run lease size");
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        throw std::runtime_error("cannot read Quidra run lease");
    }
    std::string out(static_cast<std::size_t>(end), '\0');
    std::size_t offset = 0;
    while (offset < out.size()) {
        const auto count = ::read(fd, out.data() + offset, out.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "cannot read Quidra run lease: " +
                std::string(std::strerror(errno)));
        }
        if (count == 0) break;
        offset += static_cast<std::size_t>(count);
    }
    out.resize(offset);
    return out;
#endif
}

void LockedFile::close() noexcept {
#ifdef _WIN32
    auto& handle = native_->handle;
    if (handle != INVALID_HANDLE_VALUE) {
        if (locked_) UnlockFileEx(handle, 0, 1, 0, &native_->overlapped);
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
#else
    auto& fd = native_->fd;
    if (fd >= 0) {
        if (locked_) ::flock(fd, LOCK_UN);
        ::close(fd);
        fd = -1;
    }
#endif
    locked_ = false;
}

} // namespace quidra::platform
