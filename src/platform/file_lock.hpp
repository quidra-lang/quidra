#pragma once

// LockedFile: a file opened for reading and writing and held under an
// exclusive whole-file lock until close() or destruction. Direct runs use it
// for the run registry's lock and for the lease of each temporary run
// artifact.
//
// POSIX opens the file with mode 0600 and locks it with flock(LOCK_EX);
// Windows opens it with CreateFileW, sharing read, write and delete, and
// locks its first byte with LockFileEx. Every failure throws
// std::runtime_error, except the outcomes the caller asks about:
//   - open_existing on a missing file leaves the object without a file
//     (present() is false);
//   - create_new on an existing file throws LockFileExistsError;
//   - try_only on a file another holder has locked leaves the object without
//     a lock (present() true, locked() false; the file is closed again).

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra::platform {

enum class LockOpenMode { open_always, create_new, open_existing };

class LockFileExistsError : public std::runtime_error {
public:
    LockFileExistsError() : std::runtime_error("lock file already exists") {}
};

class LockedFile {
public:
    LockedFile(const std::filesystem::path& path, LockOpenMode mode, bool try_only);
    ~LockedFile();

    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;

    bool present() const noexcept { return present_; }
    bool locked() const noexcept { return locked_; }

    // Replaces the file's content with `text` and flushes it to the device.
    // Requires the lock.
    void write_all(std::string_view text);
    // The file's whole content, at most 1 MiB. Requires the lock.
    std::string read_all();
    // Releases the lock and closes the file; idempotent.
    void close() noexcept;

private:
    struct Native;

    bool present_{false};
    bool locked_{false};
    std::unique_ptr<Native> native_;
};

} // namespace quidra::platform
