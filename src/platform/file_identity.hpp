#pragma once

// FileIdentity: what the file system says about a file without reading it,
// precise enough that a replaced or rewritten file compares unequal.
//
// POSIX takes it from stat(2): the device and inode, the size, and the
// modification and status-change times in nanoseconds. Windows opens the
// file for its attributes only and takes the volume serial number and the
// 128-bit file id (FileIdInfo), the size, and the LastWriteTime and
// ChangeTime of FileBasicInfo, in 100-nanosecond units scaled to
// nanoseconds.
//
// The change time moves whenever the inode does (a write, a chmod, a new
// hard link, a rename onto the name), so an equal identity means the file
// was neither replaced nor modified, unless something restored all of the
// fields on purpose. Symbolic links are followed.

#include <cstdint>
#include <filesystem>
#include <optional>

namespace quidra::platform {

struct FileIdentity {
    std::uint64_t device{};
    // The inode; on Windows the low and high halves of the 128-bit file id.
    std::uint64_t file{};
    std::uint64_t file_high{};
    std::uint64_t size{};
    std::int64_t modified_ns{};
    std::int64_t changed_ns{};

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

// The identity of the file `path` names, or nullopt when there is none (it
// does not exist, or its attributes cannot be read).
std::optional<FileIdentity> file_identity(const std::filesystem::path& path);

} // namespace quidra::platform
