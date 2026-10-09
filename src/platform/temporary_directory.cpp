#include "platform/temporary_directory.hpp"

#include <chrono>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace quidra::platform {

TemporaryDirectory::TemporaryDirectory(std::string_view prefix,
                                       std::string_view failure_message) {
    const auto base = std::filesystem::temp_directory_path();
    std::random_device random;
    const auto now = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto nonce =
            (static_cast<unsigned long long>(random()) << 32U) ^
            random() ^ now ^ attempt;
        path_ = base / (std::string(prefix) + std::to_string(nonce));
        std::error_code error;
        if (std::filesystem::create_directory(path_, error)) return;
    }
    throw std::runtime_error(std::string(failure_message));
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

} // namespace quidra::platform
