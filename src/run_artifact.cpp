#include "run_artifact.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cerrno>
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
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace quidra::run_artifact {
namespace {

enum class OpenMode { open_always, create_new, open_existing };

class FileExistsError : public std::runtime_error {
public:
    FileExistsError() : std::runtime_error("lock file already exists") {}
};

class LockedFile {
public:
    LockedFile(const fs::path& path, OpenMode mode, bool try_only) {
#ifdef _WIN32
        DWORD disposition = OPEN_EXISTING;
        if (mode == OpenMode::open_always) disposition = OPEN_ALWAYS;
        else if (mode == OpenMode::create_new) disposition = CREATE_NEW;
        handle_ = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (mode == OpenMode::open_existing && error == ERROR_FILE_NOT_FOUND) return;
            if (mode == OpenMode::create_new &&
                (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
                throw FileExistsError();
            }
            throw std::runtime_error(
                "cannot open Quidra run lock (Windows error " + std::to_string(error) + ")");
        }
        present_ = true;
        DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
        if (try_only) flags |= LOCKFILE_FAIL_IMMEDIATELY;
        if (!LockFileEx(handle_, flags, 0, 1, 0, &overlapped_)) {
            const DWORD error = GetLastError();
            if (try_only && error == ERROR_LOCK_VIOLATION) {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
                return;
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "cannot lock Quidra run lock (Windows error " + std::to_string(error) + ")");
        }
        locked_ = true;
#else
        int flags = O_RDWR;
        if (mode == OpenMode::open_always) flags |= O_CREAT;
        else if (mode == OpenMode::create_new) flags |= O_CREAT | O_EXCL;
        fd_ = ::open(path.c_str(), flags, 0600);
        if (fd_ < 0) {
            if (mode == OpenMode::open_existing && errno == ENOENT) return;
            if (mode == OpenMode::create_new && errno == EEXIST) throw FileExistsError();
            throw std::runtime_error(
                "cannot open Quidra run lock: " + std::string(std::strerror(errno)));
        }
        present_ = true;
        const int operation = LOCK_EX | (try_only ? LOCK_NB : 0);
        if (::flock(fd_, operation) != 0) {
            const int error = errno;
            if (try_only && (error == EWOULDBLOCK || error == EAGAIN)) {
                ::close(fd_);
                fd_ = -1;
                return;
            }
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "cannot lock Quidra run lock: " + std::string(std::strerror(error)));
        }
        locked_ = true;
#endif
    }

    ~LockedFile() { close(); }

    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;

    bool present() const noexcept { return present_; }
    bool locked() const noexcept { return locked_; }

    void write_all(std::string_view text) {
        if (!locked_) throw std::runtime_error("Quidra run lock is not held");
#ifdef _WIN32
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
            throw std::runtime_error("cannot reset Quidra run lease");
        }
        std::size_t offset = 0;
        while (offset < text.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(
                    text.size() - offset, static_cast<std::size_t>(0xffffffffu)));
            DWORD written = 0;
            if (!WriteFile(
                    handle_, text.data() + offset, chunk, &written, nullptr) ||
                written == 0) {
                throw std::runtime_error("cannot write Quidra run lease");
            }
            offset += written;
        }
        if (!FlushFileBuffers(handle_)) {
            throw std::runtime_error("cannot flush Quidra run lease");
        }
#else
        if (::ftruncate(fd_, 0) != 0 || ::lseek(fd_, 0, SEEK_SET) < 0) {
            throw std::runtime_error("cannot reset Quidra run lease");
        }
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto written = ::write(fd_, text.data() + offset, text.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(
                    "cannot write Quidra run lease: " +
                    std::string(std::strerror(errno)));
            }
            if (written == 0) throw std::runtime_error("cannot write Quidra run lease");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(fd_) != 0) throw std::runtime_error("cannot flush Quidra run lease");
#endif
    }

    std::string read_all() {
        if (!locked_) throw std::runtime_error("Quidra run lock is not held");
#ifdef _WIN32
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) ||
            size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
            throw std::runtime_error("invalid Quidra run lease size");
        }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN)) {
            throw std::runtime_error("cannot read Quidra run lease");
        }
        std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < out.size()) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(
                    out.size() - offset, static_cast<std::size_t>(0xffffffffu)));
            if (!ReadFile(handle_, out.data() + offset, chunk, &read, nullptr)) {
                throw std::runtime_error("cannot read Quidra run lease");
            }
            if (read == 0) break;
            offset += read;
        }
        out.resize(offset);
        return out;
#else
        const auto end = ::lseek(fd_, 0, SEEK_END);
        if (end < 0 || end > 1024 * 1024) {
            throw std::runtime_error("invalid Quidra run lease size");
        }
        if (::lseek(fd_, 0, SEEK_SET) < 0) {
            throw std::runtime_error("cannot read Quidra run lease");
        }
        std::string out(static_cast<std::size_t>(end), '\0');
        std::size_t offset = 0;
        while (offset < out.size()) {
            const auto count = ::read(fd_, out.data() + offset, out.size() - offset);
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

    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (locked_) UnlockFileEx(handle_, 0, 1, 0, &overlapped_);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            if (locked_) ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
#endif
        locked_ = false;
    }

private:
    bool present_{false};
    bool locked_{false};
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    OVERLAPPED overlapped_{};
#else
    int fd_{-1};
#endif
};

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
    explicit LockState(std::unique_ptr<LockedFile> value)
        : file(std::move(value)) {}
    std::unique_ptr<LockedFile> file;
};

void cleanup_stale() noexcept {
    try {
        const auto registry = ensure_registry_directory();
        LockedFile registry_lock(
            registry / "registry.lock", OpenMode::open_always, false);
        std::error_code iterator_error;
        for (fs::directory_iterator it(registry, iterator_error), end;
             !iterator_error && it != end; it.increment(iterator_error)) {
            const auto lease_path = it->path();
            if (lease_path.extension() != ".lease") continue;

            try {
                LockedFile lease(
                    lease_path, OpenMode::open_existing, true);
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
    LockedFile registry_lock(
        registry / "registry.lock", OpenMode::open_always, false);

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
            auto lock = std::make_unique<LockedFile>(
                candidate_lease, OpenMode::create_new, false);
            lock->write_all(
                hex_encode(path_bytes(candidate)) + "\n");
            executable_ = candidate;
            llvm_ = candidate_llvm;
            lease_path_ = candidate_lease;
            lease_ =
                std::make_unique<LockState>(std::move(lock));
            return;
        } catch (const FileExistsError&) {
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
