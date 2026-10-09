#pragma once

// TemporaryDirectory: a new directory under the system's temporary directory,
// removed with everything in it when the object is destroyed.
//
// The directory's name is the caller's prefix followed by a random nonce.
// Creation tries 64 nonces and then throws std::runtime_error with the
// caller's message. Removal ignores errors.

#include <filesystem>
#include <string_view>

namespace quidra::platform {

class TemporaryDirectory {
public:
    TemporaryDirectory(std::string_view prefix, std::string_view failure_message);
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory();

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace quidra::platform
