#pragma once

// File operations of a private cache directory (private_directory.hpp)
// that differ between operating systems:
//
//   sync_directory_files   flushes every regular file directly in a
//                          directory, then the directory itself, to the
//                          device (fsync; FlushFileBuffers on Windows, where
//                          a directory cannot be flushed)
//   rename_directory       moves a directory onto a name that does not exist
//                          yet, atomically (rename(2); MoveFileExW without
//                          MOVEFILE_REPLACE_EXISTING on Windows). A POSIX
//                          rename may replace an empty directory; the cache
//                          never leaves one under a published name.
//   private_file_digest    the size and SHA-256 of a regular file of ours,
//                          opened without following a symbolic link
//   allocated_size         the bytes a file occupies on the device (st_blocks
//                          * 512; GetCompressedFileSizeW on Windows)
//   touch_file             sets a file's modification time to now
//   link_or_copy_file      a hard link of a file under a new name, or a copy
//                          (mode 0700 on POSIX) where the file system cannot
//                          link it (another volume, no hard links)

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace quidra::platform {

bool sync_directory_files(const std::filesystem::path& directory);

std::error_code rename_directory(const std::filesystem::path& from, const std::filesystem::path& to);

struct PrivateFileDigest {
    std::uint64_t size{};
    std::string sha256;
};

std::optional<PrivateFileDigest> private_file_digest(const std::filesystem::path& path);

std::optional<std::uint64_t> allocated_size(const std::filesystem::path& path);

bool touch_file(const std::filesystem::path& path);

bool link_or_copy_file(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace quidra::platform
