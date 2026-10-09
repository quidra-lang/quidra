#pragma once

// Private directories: a directory tree that only the current user can
// change, so that nothing another user plants there is ever read or run.
//
// POSIX. Directories are created with mode 0700 and files with 0600,
// opened with O_NOFOLLOW. An existing directory is checked with lstat(2):
// it must be a directory (not a symbolic link) owned by the effective user,
// and it must not be writable by the group or by others. A directory that
// is ours but group- or world-writable is reset to 0700; one that belongs to
// another user is refused. The root itself may be reached through a
// symbolic link (QUIDRA_CACHE_DIR may name one): the directory it resolves
// to is checked. Below the root a symbolic link is refused.
//
// Windows. The root is created with a protected DACL that gives full
// control to the current user, SYSTEM and Administrators and inherits
// nothing; the directories below it inherit that DACL. A directory must be
// owned by the current user's SID, and a reparse point is refused.
//
// Every refusal says why, in a sentence that names the path, so that a
// command can explain it; the cached run paths never print it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace quidra::platform {

// What the POSIX checks look at in one directory entry.
struct DirectoryEntryStatus {
    enum class Kind { missing, directory, symbolic_link, other };
    Kind kind{Kind::missing};
    std::uint64_t owner{};
    // The permission bits of the mode (07777).
    std::uint32_t permissions{};
};

enum class PrivateDirectoryVerdict { private_directory, too_permissive, foreign };

// How the POSIX rules judge an existing directory for `user`: ours and not
// writable by group or others, ours but writable by them, or someone
// else's.
PrivateDirectoryVerdict judge_private_directory(
    const DirectoryEntryStatus& status, std::uint64_t user);

// A test seam: reads an entry's status in place of lstat(2). Creation and
// mode changes still act on the file system. Ignored on Windows.
using DirectoryStatusReader =
    std::function<DirectoryEntryStatus(const std::filesystem::path&)>;

struct PrivateDirectory {
    // The usable directory, with symbolic links in and above the root
    // resolved; nullopt when it is refused.
    std::optional<std::filesystem::path> path;
    std::string refusal;
};

// Creates `root` (and its missing parents) when it does not exist, then
// checks it.
PrivateDirectory open_private_root(
    const std::filesystem::path& root, const DirectoryStatusReader& read_status = {});

// Creates or checks each directory of `relative` below an opened root, in
// order. `relative` must be relative and must not leave the root.
PrivateDirectory open_private_subdirectory(
    const std::filesystem::path& root, const std::filesystem::path& relative,
    const DirectoryStatusReader& read_status = {});

// Checks each directory of `relative` below an opened root, as
// open_private_subdirectory does, but creates nothing: a missing directory
// is refused.
PrivateDirectory check_private_subdirectory(
    const std::filesystem::path& root, const std::filesystem::path& relative,
    const DirectoryStatusReader& read_status = {});

// The whole content of `path` in a private directory, at most `limit` bytes:
// a regular file of ours, opened without following a symbolic link. nullopt
// when it is missing, not a regular file, not ours, larger than `limit` or
// unreadable.
std::optional<std::string> read_private_file(
    const std::filesystem::path& path, std::size_t limit);

// Replaces `path` atomically with `content`: a new private file (0600) next
// to it is written, flushed and renamed over it. False on any failure, which
// leaves `path` as it was.
bool replace_private_file(const std::filesystem::path& path, std::string_view content);

} // namespace quidra::platform
