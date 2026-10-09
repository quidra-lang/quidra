#include "platform/private_directory.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/file_descriptor.hpp"
#endif

namespace fs = std::filesystem;

namespace quidra::platform {
namespace {

PrivateDirectory refused(std::string reason) {
    return PrivateDirectory{std::nullopt, std::move(reason)};
}

std::string quoted(const fs::path& path) {
    return "'" + path.string() + "'";
}

// A name for a temporary sibling of `path` that no other writer picks.
fs::path temporary_sibling(const fs::path& path) {
    std::random_device random;
    const auto now = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto nonce = (static_cast<unsigned long long>(random()) << 32U) ^ random() ^ now;
    return path.parent_path() /
           ("." + path.filename().string() + ".tmp-" + std::to_string(nonce));
}

bool escapes(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return true;
    for (const auto& component : relative.lexically_normal()) {
        if (component == "..") return true;
    }
    return false;
}

} // namespace

PrivateDirectoryVerdict judge_private_directory(
    const DirectoryEntryStatus& status, std::uint64_t user) {
    if (status.owner != user) return PrivateDirectoryVerdict::foreign;
    if ((status.permissions & 022U) != 0) return PrivateDirectoryVerdict::too_permissive;
    return PrivateDirectoryVerdict::private_directory;
}

#ifdef _WIN32

namespace {

// The SID of the user this process runs as, in a buffer that holds it.
std::vector<unsigned char> current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    std::vector<unsigned char> sid;
    if (size != 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        const DWORD length = GetLengthSid(user->User.Sid);
        sid.resize(length);
        if (!CopySid(length, sid.data(), user->User.Sid)) sid.clear();
    }
    CloseHandle(token);
    return sid;
}

bool owned_by(const fs::path& path, const std::vector<unsigned char>& sid) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
            nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    const bool equal =
        owner && !sid.empty() &&
        EqualSid(owner, const_cast<unsigned char*>(sid.data())) != FALSE;
    LocalFree(descriptor);
    return equal;
}

// Creates `path` with a protected DACL for the current user, SYSTEM and
// Administrators, inherited by everything created below it.
bool create_protected_directory(const fs::path& path, const std::vector<unsigned char>& sid) {
    LPWSTR text = nullptr;
    if (sid.empty() ||
        !ConvertSidToStringSidW(const_cast<unsigned char*>(sid.data()), &text)) {
        return false;
    }
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;FA;;;") + text +
                              L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    LocalFree(text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE ||
                         GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(descriptor);
    return created;
}

std::string check_directory(
    const fs::path& path, const std::vector<unsigned char>& sid, bool allow_reparse_point) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return "cache directory " + quoted(path) + " cannot be inspected";
    }
    if (!allow_reparse_point && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return "cache directory " + quoted(path) + " is a reparse point";
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return "cache directory " + quoted(path) + " is not a directory";
    }
    if (!owned_by(path, sid)) {
        return "cache directory " + quoted(path) + " is owned by another user";
    }
    return {};
}

} // namespace

PrivateDirectory open_private_root(const fs::path& root, const DirectoryStatusReader&) {
    if (!root.is_absolute()) {
        return refused("cache directory " + quoted(root) + " is not an absolute path");
    }
    const auto sid = current_user_sid();
    if (sid.empty()) return refused("cannot identify the current user");
    std::error_code error;
    if (!fs::exists(root, error)) {
        fs::create_directories(root.parent_path(), error);
        if (!create_protected_directory(root, sid)) {
            return refused("cannot create cache directory " + quoted(root));
        }
    }
    const auto resolved = fs::canonical(root, error);
    if (error) return refused("cannot resolve cache directory " + quoted(root));
    if (auto reason = check_directory(resolved, sid, true); !reason.empty()) {
        return refused(std::move(reason));
    }
    return PrivateDirectory{resolved, {}};
}

namespace {

PrivateDirectory walk_private_subdirectory(
    const fs::path& root, const fs::path& relative, bool create) {
    if (escapes(relative)) {
        return refused("cache subdirectory " + quoted(relative) + " leaves the cache directory");
    }
    const auto sid = current_user_sid();
    if (sid.empty()) return refused("cannot identify the current user");
    fs::path current = root;
    for (const auto& component : relative.lexically_normal()) {
        if (component.empty() || component == ".") continue;
        current /= component;
        if (GetFileAttributesW(current.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!create) return refused("cache directory " + quoted(current) + " does not exist");
            if (!CreateDirectoryW(current.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return refused("cannot create cache directory " + quoted(current));
            }
        }
        if (auto reason = check_directory(current, sid, false); !reason.empty()) {
            return refused(std::move(reason));
        }
    }
    return PrivateDirectory{current, {}};
}

} // namespace

PrivateDirectory open_private_subdirectory(
    const fs::path& root, const fs::path& relative, const DirectoryStatusReader&) {
    return walk_private_subdirectory(root, relative, true);
}

PrivateDirectory check_private_subdirectory(
    const fs::path& root, const fs::path& relative, const DirectoryStatusReader&) {
    return walk_private_subdirectory(root, relative, false);
}

std::optional<std::string> read_private_file(const fs::path& path, std::size_t limit) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return std::nullopt;
    }
    if (!owned_by(path, current_user_sid())) return std::nullopt;
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<std::string> content;
    if (GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
        static_cast<unsigned long long>(size.QuadPart) <= limit) {
        std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t done = 0;
        bool ok = true;
        while (done < data.size()) {
            DWORD read = 0;
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(data.size() - done, 1U << 20U));
            if (!ReadFile(handle, data.data() + done, chunk, &read, nullptr) || read == 0) {
                ok = false;
                break;
            }
            done += read;
        }
        if (ok) content = std::move(data);
    }
    CloseHandle(handle);
    return content;
}

bool replace_private_file(const fs::path& path, std::string_view content) {
    const auto temporary = temporary_sibling(path);
    const HANDLE handle = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < content.size()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(content.size() - done, 1U << 20U));
        ok = WriteFile(handle, content.data() + done, chunk, &written, nullptr) && written != 0;
        done += written;
    }
    ok = ok && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (ok) {
        ok = MoveFileExW(
                 temporary.c_str(), path.c_str(),
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

#else

namespace {

DirectoryEntryStatus entry_status(const fs::path& path) {
    struct stat status {};
    DirectoryEntryStatus result;
    if (::lstat(path.c_str(), &status) != 0) return result;
    if (S_ISLNK(status.st_mode)) result.kind = DirectoryEntryStatus::Kind::symbolic_link;
    else if (S_ISDIR(status.st_mode)) result.kind = DirectoryEntryStatus::Kind::directory;
    else result.kind = DirectoryEntryStatus::Kind::other;
    result.owner = static_cast<std::uint64_t>(status.st_uid);
    result.permissions = static_cast<std::uint32_t>(status.st_mode & 07777);
    return result;
}

DirectoryEntryStatus read_entry(const fs::path& path, const DirectoryStatusReader& read_status) {
    return read_status ? read_status(path) : entry_status(path);
}

// Empty when `path` is a private directory of ours (after resetting the
// permissions of one that is too permissive); otherwise why it is not.
std::string check_directory(const fs::path& path, const DirectoryStatusReader& read_status) {
    const auto status = read_entry(path, read_status);
    switch (status.kind) {
        case DirectoryEntryStatus::Kind::missing:
            return "cache directory " + quoted(path) + " does not exist";
        case DirectoryEntryStatus::Kind::symbolic_link:
            return "cache directory " + quoted(path) + " is a symbolic link";
        case DirectoryEntryStatus::Kind::other:
            return "cache directory " + quoted(path) + " is not a directory";
        case DirectoryEntryStatus::Kind::directory:
            break;
    }
    switch (judge_private_directory(status, static_cast<std::uint64_t>(::geteuid()))) {
        case PrivateDirectoryVerdict::foreign:
            return "cache directory " + quoted(path) + " is owned by another user";
        case PrivateDirectoryVerdict::too_permissive:
            if (::chmod(path.c_str(), 0700) != 0) {
                return "cannot restrict the permissions of cache directory " + quoted(path) +
                       ": " + std::strerror(errno);
            }
            return {};
        case PrivateDirectoryVerdict::private_directory:
            return {};
    }
    return {};
}

std::string make_directory(const fs::path& path) {
    if (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) return {};
    return "cannot create cache directory " + quoted(path) + ": " + std::strerror(errno);
}

} // namespace

PrivateDirectory open_private_root(const fs::path& root, const DirectoryStatusReader& read_status) {
    if (!root.is_absolute()) {
        return refused("cache directory " + quoted(root) + " is not an absolute path");
    }
    std::error_code error;
    if (!fs::exists(root, error)) {
        fs::create_directories(root.parent_path(), error);
        if (auto reason = make_directory(root); !reason.empty()) return refused(std::move(reason));
    }
    const auto resolved = fs::canonical(root, error);
    if (error) {
        return refused("cannot resolve cache directory " + quoted(root) + ": " + error.message());
    }
    if (auto reason = check_directory(resolved, read_status); !reason.empty()) {
        return refused(std::move(reason));
    }
    return PrivateDirectory{resolved, {}};
}

namespace {

PrivateDirectory walk_private_subdirectory(
    const fs::path& root, const fs::path& relative, const DirectoryStatusReader& read_status,
    bool create) {
    if (escapes(relative)) {
        return refused("cache subdirectory " + quoted(relative) + " leaves the cache directory");
    }
    fs::path current = root;
    for (const auto& component : relative.lexically_normal()) {
        if (component.empty() || component == ".") continue;
        current /= component;
        if (create &&
            read_entry(current, read_status).kind == DirectoryEntryStatus::Kind::missing) {
            if (auto reason = make_directory(current); !reason.empty()) {
                return refused(std::move(reason));
            }
        }
        if (auto reason = check_directory(current, read_status); !reason.empty()) {
            return refused(std::move(reason));
        }
    }
    return PrivateDirectory{current, {}};
}

} // namespace

PrivateDirectory open_private_subdirectory(
    const fs::path& root, const fs::path& relative, const DirectoryStatusReader& read_status) {
    return walk_private_subdirectory(root, relative, read_status, true);
}

PrivateDirectory check_private_subdirectory(
    const fs::path& root, const fs::path& relative, const DirectoryStatusReader& read_status) {
    return walk_private_subdirectory(root, relative, read_status, false);
}

std::optional<std::string> read_private_file(const fs::path& path, std::size_t limit) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct stat status {};
    std::optional<std::string> content;
    if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == ::geteuid() && status.st_size >= 0 &&
        static_cast<unsigned long long>(status.st_size) <= limit) {
        std::string data(static_cast<std::size_t>(status.st_size), '\0');
        char extra = 0;
        // The file must end where fstat said it does.
        if (read_exact(fd, data.data(), data.size()) && ::read(fd, &extra, 1) == 0) {
            content = std::move(data);
        }
    }
    ::close(fd);
    return content;
}

bool replace_private_file(const fs::path& path, std::string_view content) {
    const auto temporary = temporary_sibling(path);
    const int fd = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, content) && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) ::unlink(temporary.c_str());
    return ok;
}

#endif

} // namespace quidra::platform
