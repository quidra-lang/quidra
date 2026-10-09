#include "run_artifact.hpp"

#include "platform/file_lock.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::run_artifact {
namespace {

fs::path registry_directory() {
#ifdef _WIN32
    return fs::temp_directory_path() / "quidra-run-registry";
#else
    return fs::temp_directory_path() /
        ("quidra-run-registry-" + std::to_string(::getuid()));
#endif
}

fs::path ensure_registry_directory() {
    const auto directory = registry_directory();
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        throw std::runtime_error(
            "cannot create Quidra run registry: " + ec.message());
    }
#ifndef _WIN32
    fs::permissions(
        directory, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) {
        throw std::runtime_error(
            "cannot secure Quidra run registry: " + ec.message());
    }
#endif
    return directory;
}

std::string path_bytes(const fs::path& path) {
#ifdef _WIN32
    const auto& wide = path.native();
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("cannot encode Quidra run artifact path");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
            static_cast<int>(wide.size()), out.data(), needed,
            nullptr, nullptr) != needed) {
        throw std::runtime_error("cannot encode Quidra run artifact path");
    }
    return out;
#else
    return path.native();
#endif
}

fs::path path_from_bytes(const std::string& bytes) {
#ifdef _WIN32
    if (bytes.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("cannot decode Quidra run artifact path");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), wide.data(), needed) != needed) {
        throw std::runtime_error("cannot decode Quidra run artifact path");
    }
    return fs::path(wide);
#else
    return fs::path(bytes);
#endif
}

std::string hex_encode(std::string_view bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xf]);
    }
    return out;
}

std::optional<std::string> hex_decode(std::string_view text) {
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.size() % 2 != 0) return std::nullopt;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = value(text[i]);
        const int low = value(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string make_run_id(unsigned attempt) {
    std::random_device random;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::ostringstream out;
    out << std::hex << now << '-' << process_id() << '-'
        << static_cast<std::uint64_t>(random())
        << static_cast<std::uint64_t>(random())
        << '-' << attempt;
    return out.str();
}

bool is_managed_artifact(const fs::path& path) {
    if (!path.is_absolute()) return false;
    const auto name = path.filename().string();
    return name.rfind(".quidra-run-", 0) == 0;
}

bool remove_file_if_present(const fs::path& path) noexcept {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec) return false;
    if (!fs::exists(status)) return true;
    if (fs::is_directory(status)) return false;
    fs::remove(path, ec);
    return !ec;
}

} // namespace

struct TemporaryArtifact::LockState {
    explicit LockState(std::unique_ptr<platform::LockedFile> value)
        : file(std::move(value)) {}
    std::unique_ptr<platform::LockedFile> file;
};

void cleanup_stale() noexcept {
    try {
        const auto registry = ensure_registry_directory();
        platform::LockedFile registry_lock(
            registry / "registry.lock", platform::LockOpenMode::open_always, false);
        std::error_code iterator_error;
        for (fs::directory_iterator it(registry, iterator_error), end;
             !iterator_error && it != end; it.increment(iterator_error)) {
            const auto lease_path = it->path();
            if (lease_path.extension() != ".lease") continue;

            try {
                platform::LockedFile lease(
                    lease_path, platform::LockOpenMode::open_existing, true);
                if (!lease.present() || !lease.locked()) continue;

                bool can_remove_lease = false;
                const auto decoded = hex_decode(lease.read_all());
                if (!decoded) {
                    can_remove_lease = true;
                } else {
                    const auto artifact = path_from_bytes(*decoded);
                    if (!is_managed_artifact(artifact)) {
                        can_remove_lease = true;
                    } else {
                        auto llvm = artifact;
                        llvm += ".ll";
                        const bool artifact_removed =
                            remove_file_if_present(artifact);
                        const bool llvm_removed =
                            remove_file_if_present(llvm);
                        can_remove_lease =
                            artifact_removed && llvm_removed;
                    }
                }

                lease.close();
                if (can_remove_lease) {
                    std::error_code remove_error;
                    fs::remove(lease_path, remove_error);
                }
            } catch (...) {
                // One damaged/inaccessible lease must not block startup.
            }
        }
    } catch (...) {
        // Startup cleanup is best-effort. Creating a new managed artifact still
        // reports registry errors when direct execution is requested.
    }
}

TemporaryArtifact::TemporaryArtifact(const fs::path& source) {
    const auto source_absolute = fs::absolute(source);
    const auto source_directory = source_absolute.parent_path();
    const auto registry = ensure_registry_directory();
    platform::LockedFile registry_lock(
        registry / "registry.lock", platform::LockOpenMode::open_always, false);

    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto run_id = make_run_id(attempt);
#ifdef _WIN32
        const auto candidate =
            source_directory / (".quidra-run-" + run_id + ".exe");
#else
        const auto candidate =
            source_directory / (".quidra-run-" + run_id);
#endif
        auto candidate_llvm = candidate;
        candidate_llvm += ".ll";
        std::error_code ec;
        if (fs::exists(candidate, ec) ||
            fs::exists(candidate_llvm, ec)) {
            continue;
        }

        const auto candidate_lease =
            registry / (run_id + ".lease");
        try {
            auto lock = std::make_unique<platform::LockedFile>(
                candidate_lease, platform::LockOpenMode::create_new, false);
            lock->write_all(
                hex_encode(path_bytes(candidate)) + "\n");
            executable_ = candidate;
            llvm_ = candidate_llvm;
            lease_path_ = candidate_lease;
            lease_ =
                std::make_unique<LockState>(std::move(lock));
            return;
        } catch (const platform::LockFileExistsError&) {
            continue;
        } catch (...) {
            std::error_code remove_error;
            fs::remove(candidate_lease, remove_error);
            throw;
        }
    }
    throw std::runtime_error(
        "cannot reserve a unique Quidra temporary run artifact");
}

TemporaryArtifact::~TemporaryArtifact() {
    const bool executable_removed =
        remove_file_if_present(executable_);
    const bool llvm_removed =
        remove_file_if_present(llvm_);
    if (lease_ && lease_->file) lease_->file->close();
    if (executable_removed && llvm_removed &&
        !lease_path_.empty()) {
        std::error_code ec;
        fs::remove(lease_path_, ec);
    }
}

} // namespace quidra::run_artifact
