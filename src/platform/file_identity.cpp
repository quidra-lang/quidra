#include "platform/file_identity.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace quidra::platform {

#ifdef _WIN32

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
    identity.modified_ns = basic.LastWriteTime.QuadPart * 100;
    identity.changed_ns = basic.ChangeTime.QuadPart * 100;
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
