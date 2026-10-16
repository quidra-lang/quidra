#include "platform/file_identity.hpp"

#include <limits>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace quidra::platform {

#ifdef _WIN32

namespace {
// Windows FILETIME counts 100ns ticks from 1601. Scaling that absolute
// epoch directly into signed nanoseconds overflows in the present day.
// Convert to Unix-relative ticks first, and clamp out-of-range dates.
std::int64_t filetime_unix_ns(const LARGE_INTEGER& timestamp) noexcept {
    constexpr std::int64_t unix_epoch_ticks = 116444736000000000LL;
    if (timestamp.QuadPart <= unix_epoch_ticks) return 0;
    const auto unix_ticks = timestamp.QuadPart - unix_epoch_ticks;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    if (unix_ticks > limit / 100) return limit;
    return unix_ticks * 100;
}
} // namespace

std::optional<FileIdentity> file_identity(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ID_INFO id{};
    FILE_BASIC_INFO basic{};
    LARGE_INTEGER size{};
    const bool ok =
        GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileSizeEx(handle, &size);
    CloseHandle(handle);
    if (!ok) return std::nullopt;

    FileIdentity identity;
    identity.device = id.VolumeSerialNumber;
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    for (int i = 0; i < 8; ++i) {
        low |= static_cast<std::uint64_t>(id.FileId.Identifier[i]) << (8 * i);
        high |= static_cast<std::uint64_t>(id.FileId.Identifier[8 + i]) << (8 * i);
    }
    identity.file = low;
    identity.file_high = high;
    identity.size = static_cast<std::uint64_t>(size.QuadPart);
    identity.modified_ns = filetime_unix_ns(basic.LastWriteTime);
    identity.changed_ns = filetime_unix_ns(basic.ChangeTime);
    return identity;
}

#else

namespace {

std::int64_t nanoseconds(const struct timespec& time) {
    return static_cast<std::int64_t>(time.tv_sec) * 1000000000 +
           static_cast<std::int64_t>(time.tv_nsec);
}

} // namespace

std::optional<FileIdentity> file_identity(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) return std::nullopt;
    FileIdentity identity;
    identity.device = static_cast<std::uint64_t>(status.st_dev);
    identity.file = static_cast<std::uint64_t>(status.st_ino);
    identity.size = static_cast<std::uint64_t>(status.st_size);
#ifdef __APPLE__
    identity.modified_ns = nanoseconds(status.st_mtimespec);
    identity.changed_ns = nanoseconds(status.st_ctimespec);
#else
    identity.modified_ns = nanoseconds(status.st_mtim);
    identity.changed_ns = nanoseconds(status.st_ctim);
#endif
    return identity;
}

#endif

} // namespace quidra::platform
