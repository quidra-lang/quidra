// The user cache directory and the private directories under it: the
// per-OS location rules, private creation and checks (permissions, symbolic
// links, foreign owners through an injected status), private files, file
// identity and the OS version text.

#include "platform/environment.hpp"
#include "platform/file_identity.hpp"
#include "platform/os_version.hpp"
#include "platform/private_directory.hpp"
#include "platform/user_cache_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace quidra::platform;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string shown(const std::optional<fs::path>& path) {
    return path ? path->generic_string() : std::string("<none>");
}

void expect_root(CacheDirectoryConvention convention, const CacheDirectoryInputs& inputs,
                 const std::optional<fs::path>& expected, const std::string& name) {
    const auto actual = user_cache_directory(convention, inputs);
    check(actual == expected,
          name + ": expected " + shown(expected) + ", got " + shown(actual));
}

void location_rules() {
    using C = CacheDirectoryConvention;
    CacheDirectoryInputs inputs;
    inputs.working_directory = "/work/project";

    inputs.home = "/home/ada";
    expect_root(C::macos, inputs, fs::path("/home/ada/Library/Caches/Quidra"), "macOS home");
    expect_root(C::xdg, inputs, fs::path("/home/ada/.cache/quidra"), "XDG home");
    expect_root(C::windows, inputs, std::nullopt, "Windows without the known folder");

    inputs.xdg_cache_home = "/var/cache/ada";
    expect_root(C::xdg, inputs, fs::path("/var/cache/ada/quidra"), "XDG_CACHE_HOME");
    expect_root(C::macos, inputs, fs::path("/home/ada/Library/Caches/Quidra"),
                "macOS ignores XDG_CACHE_HOME");
    inputs.xdg_cache_home = "relative/cache";
    expect_root(C::xdg, inputs, fs::path("/home/ada/.cache/quidra"),
                "a relative XDG_CACHE_HOME is ignored");
    inputs.xdg_cache_home = "";
    expect_root(C::xdg, inputs, fs::path("/home/ada/.cache/quidra"),
                "an empty XDG_CACHE_HOME is ignored");
    inputs.xdg_cache_home.reset();

    inputs.home.reset();
    expect_root(C::macos, inputs, std::nullopt, "macOS without HOME");
    expect_root(C::xdg, inputs, std::nullopt, "XDG without HOME");
    inputs.home = "";
    expect_root(C::macos, inputs, std::nullopt, "macOS with an empty HOME");
    expect_root(C::xdg, inputs, std::nullopt, "XDG with an empty HOME");
    inputs.home = "relative/home";
    expect_root(C::xdg, inputs, std::nullopt, "XDG with a relative HOME");
    inputs.xdg_cache_home = "/xdg";
    expect_root(C::xdg, inputs, fs::path("/xdg/quidra"),
                "XDG_CACHE_HOME without a usable HOME");
    inputs.xdg_cache_home.reset();

    inputs.local_app_data = fs::path("/profiles/ada/AppData/Local");
    expect_root(C::windows, inputs, fs::path("/profiles/ada/AppData/Local/Quidra/Cache"),
                "Windows known folder");
    inputs.local_app_data = fs::path("relative");
    expect_root(C::windows, inputs, std::nullopt, "a relative known folder");
    inputs.local_app_data.reset();

    for (const auto convention : {C::macos, C::xdg, C::windows}) {
        auto configured = inputs;
        configured.home = "/home/ada";
        configured.quidra_cache_dir = "/tmp/quidra-cache/../q";
        expect_root(convention, configured, fs::path("/tmp/q"), "absolute QUIDRA_CACHE_DIR");
        configured.quidra_cache_dir = "cache/dir";
        expect_root(convention, configured, fs::path("/work/project/cache/dir"),
                    "relative QUIDRA_CACHE_DIR");
        configured.quidra_cache_dir = "";
        const auto fallback = user_cache_directory(convention, configured);
        configured.quidra_cache_dir.reset();
        check(fallback == user_cache_directory(convention, configured),
              "an empty QUIDRA_CACHE_DIR selects the convention");
    }
}

#ifndef _WIN32

bool set_environment(const char* name, const std::optional<std::string>& value) {
    return value ? ::setenv(name, value->c_str(), 1) == 0 : ::unsetenv(name) == 0;
}

void process_cache_directory(const fs::path& scratch) {
    const auto saved = environment_value("QUIDRA_CACHE_DIR");
    set_environment("QUIDRA_CACHE_DIR", (scratch / "configured").string());
    check(user_cache_directory() == (scratch / "configured").lexically_normal(),
          "QUIDRA_CACHE_DIR names this process's cache directory");
    set_environment("QUIDRA_CACHE_DIR", saved);
}

std::uint32_t permissions(const fs::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) return 0xffffffffU;
    return static_cast<std::uint32_t>(status.st_mode & 07777);
}

void private_directories(const fs::path& scratch) {
    using Kind = DirectoryEntryStatus::Kind;
    const auto me = static_cast<std::uint64_t>(::geteuid());
    check(judge_private_directory({Kind::directory, me, 0700}, me) ==
              PrivateDirectoryVerdict::private_directory,
          "0700 is private");
    check(judge_private_directory({Kind::directory, me, 0755}, me) ==
              PrivateDirectoryVerdict::private_directory,
          "0755 is not writable by others");
    check(judge_private_directory({Kind::directory, me, 0770}, me) ==
              PrivateDirectoryVerdict::too_permissive,
          "group-writable is too permissive");
    check(judge_private_directory({Kind::directory, me, 0702}, me) ==
              PrivateDirectoryVerdict::too_permissive,
          "world-writable is too permissive");
    check(judge_private_directory({Kind::directory, me + 1, 0700}, me) ==
              PrivateDirectoryVerdict::foreign,
          "another user's directory is foreign");

    const auto root = scratch / "nested" / "root";
    const auto opened = open_private_root(root);
    check(opened.path.has_value(), "a new root opens: " + opened.refusal);
    check(permissions(root) == 0700, "a new root has mode 0700");

    ::chmod(root.c_str(), 0777);
    const auto reset = open_private_root(root);
    check(reset.path.has_value(), "a 0777 root of ours opens: " + reset.refusal);
    check(permissions(root) == 0700, "a 0777 root is reset to 0700");

    const auto canonical_root = fs::canonical(root);
    const auto entry = open_private_subdirectory(canonical_root, "run/0.5.0/macos-arm64");
    check(entry.path == canonical_root / "run" / "0.5.0" / "macos-arm64",
          "subdirectories are created: " + entry.refusal);
    check(permissions(canonical_root / "run") == 0700 &&
              permissions(canonical_root / "run" / "0.5.0") == 0700,
          "subdirectories have mode 0700");
    ::chmod((canonical_root / "run").c_str(), 0775);
    check(open_private_subdirectory(canonical_root, "run").path.has_value() &&
              permissions(canonical_root / "run") == 0700,
          "a group-writable subdirectory of ours is reset to 0700");

    fs::create_directories(scratch / "elsewhere");
    fs::create_directory_symlink(scratch / "elsewhere", canonical_root / "link");
    const auto through_link = open_private_subdirectory(canonical_root, "link/x");
    check(!through_link.path && through_link.refusal.find("symbolic link") != std::string::npos,
          "a symbolic link below the root is refused: " + through_link.refusal);
    check(!fs::exists(scratch / "elsewhere" / "x"), "nothing is created through the link");
    check(!open_private_subdirectory(canonical_root, "../escape").path,
          "a subdirectory may not leave the root");

    fs::create_directory_symlink(canonical_root, scratch / "root-link");
    const auto linked = open_private_root(scratch / "root-link");
    check(linked.path == canonical_root,
          "a root reached through a symbolic link resolves to its directory: " + linked.refusal);

    std::ofstream(scratch / "plain-file") << "x";
    check(!open_private_root(scratch / "plain-file").path, "a file is not a cache root");

    const auto foreign = [&](const fs::path& path) {
        DirectoryEntryStatus status;
        struct stat real {};
        if (::lstat(path.c_str(), &real) != 0) return status;
        status.kind = Kind::directory;
        status.owner = me + 1;
        status.permissions = 0700;
        return status;
    };
    const auto refused_root = open_private_root(root, foreign);
    check(!refused_root.path &&
              refused_root.refusal.find("owned by another user") != std::string::npos,
          "a root owned by another user is refused: " + refused_root.refusal);
    const auto refused_entry = open_private_subdirectory(canonical_root, "run", foreign);
    check(!refused_entry.path, "a subdirectory owned by another user is refused");

    const auto file = canonical_root / "memo.json";
    check(replace_private_file(file, "first"), "a private file is written");
    check(permissions(file) == 0600, "a private file has mode 0600");
    check(replace_private_file(file, "second, longer"), "a private file is replaced");
    check(read_private_file(file, 1024) == std::optional<std::string>("second, longer"),
          "a private file reads back");
    check(!read_private_file(file, 4), "a private file above the limit is not read");
    check(read_private_file(canonical_root / "missing", 1024) == std::nullopt,
          "a missing private file is not read");
    fs::create_symlink(file, canonical_root / "memo-link");
    check(!read_private_file(canonical_root / "memo-link", 1024),
          "a private file is not read through a symbolic link");
    check(replace_private_file(canonical_root / "empty", "") &&
              read_private_file(canonical_root / "empty", 1024) == std::optional<std::string>(""),
          "an empty private file reads back");
    std::size_t leftovers = 0;
    for (const auto& item : fs::directory_iterator(canonical_root)) {
        if (item.path().filename().string().find(".tmp-") != std::string::npos) ++leftovers;
    }
    check(leftovers == 0, "replacing private files leaves no temporary files");
}

void file_identities(const fs::path& scratch) {
    const auto file = scratch / "identity.txt";
    std::ofstream(file, std::ios::binary) << "abcd";
    const auto first = file_identity(file);
    check(first.has_value(), "a file has an identity");
    check(first == file_identity(file), "an unchanged file keeps its identity");
    check(first && first->size == 4, "the identity records the size");
    check(!file_identity(scratch / "no-such-file"), "a missing file has no identity");

    struct stat before {};
    ::stat(file.c_str(), &before);
    // Rewrite the same number of bytes and restore the modification time:
    // the status-change time still moves.
    ::usleep(20000);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "wxyz";
#ifdef __APPLE__
    const struct timespec times[2] = {before.st_atimespec, before.st_mtimespec};
#else
    const struct timespec times[2] = {before.st_atim, before.st_mtim};
#endif
    ::utimensat(AT_FDCWD, file.c_str(), times, 0);
    const auto rewritten = file_identity(file);
    check(rewritten && first && rewritten->size == first->size,
          "the rewrite keeps the size");
    check(rewritten && first && rewritten->changed_ns != first->changed_ns,
          "a rewrite with the old modification time changes the identity");

    const auto link = scratch / "identity-link.txt";
    ::usleep(20000);
    fs::create_hard_link(file, link);
    check(file_identity(file) == file_identity(link), "hard links share one identity");
    check(file_identity(file) != rewritten, "a new hard link changes the status-change time");
}

#endif

} // namespace

int main() {
    location_rules();

    const auto version = os_version();
    check(!version.empty(), "the OS version is known");

#ifndef _WIN32
    auto pattern = (fs::temp_directory_path() / "quidra-cache-tests-XXXXXX").string();
    const char* made = ::mkdtemp(pattern.data());
    if (!made) {
        std::cerr << "cannot create a scratch directory\n";
        return 1;
    }
    const fs::path scratch = fs::canonical(made);
    process_cache_directory(scratch);
    private_directories(scratch);
    file_identities(scratch);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
#endif

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "platform cache directory tests passed (" << version << ")\n";
    return 0;
}
