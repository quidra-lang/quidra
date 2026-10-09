#pragma once

// Where the run cache keeps its entries, and how an entry comes into being
// and goes away without locks on the run path.
//
// Below a private cache root (private_directory.hpp):
//
//   run/<compiler version>/<target>/<key>/   one entry: the program
//                                            (program, or program.exe),
//                                            metadata.json, build.log and the
//                                            empty `used` stamp
//   run/<compiler version>/<target>/index/<pre-key>.json
//   staging/<id>/, staging/<id>.lease        builds in progress
//   trash/                                   entries being deleted
//
// A build fills staging/<id>/ while it holds staging/<id>.lease (a
// platform::LockedFile), so that a scan can tell a live staging directory
// from the leftovers of a process that died. publish() flushes the files
// and the directory to the device and renames the directory to
// run/<version>/<target>/<key>, which is atomic on one volume, so a reader
// sees either no entry or a complete one, and never a file that changes.
// When the name already exists, another process published the same key
// first: both entries are equal by construction, so ours is discarded.
// When run/ disappeared in between (a concurrent `quidra cache clean`), the
// directories are made again and the rename is retried once. On Windows a
// rename refused by a sharing violation (an antivirus scanner holding the
// new executable) is retried with back-off for at most a second.
//
// The index of a pre-key is replaced as a whole (a new private file renamed
// over it); two concurrent updates lose one key at worst, which costs a
// later miss.
//
// discard() renames an entry into trash/ before deleting it, so a reader
// that has verified it sees it disappear as a whole; a file that cannot be
// deleted yet (a running program's image on Windows) stays in trash/ until
// a later scan.
//
// RunCacheFileSystem is the seam through which tests make renames fail or
// race and stop a publish at each of its phases.

#include "platform/file_lock.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace quidra::toolchain {

struct RunCacheLayout {
    std::filesystem::path root;
    std::string version;
    std::string target;

    std::filesystem::path run() const { return root / "run"; }
    std::filesystem::path entries() const { return run() / version / target; }
    std::filesystem::path entry(std::string_view key) const { return entries() / std::string(key); }
    std::filesystem::path index_directory() const { return entries() / "index"; }
    std::filesystem::path index(std::string_view pre_key) const {
        return index_directory() / (std::string(pre_key) + ".json");
    }
    std::filesystem::path staging() const { return root / "staging"; }
    std::filesystem::path trash() const { return root / "trash"; }
    // The paths below the root, as open_private_subdirectory takes them.
    std::filesystem::path entries_relative() const {
        return std::filesystem::path("run") / version / target;
    }
};

// The file names inside an entry.
inline constexpr std::string_view run_cache_metadata_file = "metadata.json";
inline constexpr std::string_view run_cache_build_log_file = "build.log";
inline constexpr std::string_view run_cache_used_file = "used";
std::string run_cache_program_file();

enum class RunCachePublishPhase { staged, synced, renamed };

class RunCacheFileSystem {
public:
    virtual ~RunCacheFileSystem();
    // Moves the directory `from` to the new name `to`.
    virtual std::error_code rename_directory(
        const std::filesystem::path& from, const std::filesystem::path& to);
    // Called when a publish reaches `phase`, before what follows it.
    virtual void reached(RunCachePublishPhase phase);
};

class RunCacheStaging {
public:
    ~RunCacheStaging();
    RunCacheStaging(const RunCacheStaging&) = delete;
    RunCacheStaging& operator=(const RunCacheStaging&) = delete;

    const std::filesystem::path& directory() const { return directory_; }
    const std::string& id() const { return id_; }

private:
    friend class RunCacheStore;
    RunCacheStaging() = default;

    std::string id_;
    std::filesystem::path directory_;
    std::filesystem::path lease_path_;
    std::unique_ptr<platform::LockedFile> lease_;
};

enum class RunCachePublishOutcome { published, already_present, failed };

class RunCacheStore {
public:
    // Opens the root (creating it and its staging and trash directories
    // when they are missing) with the private-directory checks. nullopt,
    // with the reason in `refusal`, when the root cannot be used.
    static std::optional<RunCacheStore> open(
        const std::filesystem::path& root, std::string version, std::string target,
        std::string* refusal = nullptr, RunCacheFileSystem* file_system = nullptr);

    const RunCacheLayout& layout() const { return layout_; }

    // A new staging directory with its lease held; nullptr when it cannot
    // be made.
    std::unique_ptr<RunCacheStaging> create_staging() const;

    RunCachePublishOutcome publish(RunCacheStaging& staging, std::string_view key) const;

    // The published entry `key`, when it exists and every directory on the
    // way passes the private-directory checks; nothing is created.
    std::optional<std::filesystem::path> entry_directory(std::string_view key) const;

    std::vector<std::string> index_keys(std::string_view pre_key) const;
    bool add_to_index(std::string_view pre_key, std::string_view key) const;

    // Moves the entry to the trash and deletes it there.
    void discard(std::string_view key) const;
    // Moves any directory below the root to the trash and deletes it there;
    // false when it could not be moved.
    bool discard_directory(const std::filesystem::path& directory) const;
    // Removes the staging directories whose lease no process holds.
    void remove_dead_staging() const;
    // Deletes what the trash holds; returns how many files could not be
    // deleted yet.
    std::uint64_t empty_trash() const;

private:
    RunCacheStore() = default;

    RunCacheLayout layout_;
    RunCacheFileSystem* file_system_{};
};

} // namespace quidra::toolchain
