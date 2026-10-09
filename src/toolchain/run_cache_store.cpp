#include "toolchain/run_cache_store.hpp"

#include "platform/cache_files.hpp"
#include "platform/file_names.hpp"
#include "platform/private_directory.hpp"
#include "toolchain/run_cache_key.hpp"
#include "toolchain/run_cache_metadata.hpp"

#include <chrono>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

constexpr std::size_t index_limit_bytes = 64 * 1024;

std::string random_id() {
    std::random_device random;
    std::ostringstream out;
    out << std::hex;
    for (int index = 0; index < 4; ++index) out << static_cast<std::uint32_t>(random());
    return out.str();
}

RunCacheFileSystem& default_file_system() {
    static RunCacheFileSystem file_system;
    return file_system;
}

enum class RenameFailure { exists, parent_missing, busy, other };

RenameFailure classify(const std::error_code& error) {
    if (error == std::errc::file_exists || error == std::errc::directory_not_empty) {
        return RenameFailure::exists;
    }
    if (error == std::errc::no_such_file_or_directory) return RenameFailure::parent_missing;
#ifdef _WIN32
    if (error == std::errc::permission_denied) return RenameFailure::busy;
#endif
    return RenameFailure::other;
}

void remove_tree(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

// Deletes `path` and everything below it; returns the files that could not
// be deleted.
std::uint64_t delete_tree(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
    if (!error) return 0;
    std::uint64_t left = 0;
    std::error_code walk_error;
    const auto status = fs::symlink_status(path, walk_error);
    if (walk_error || status.type() == fs::file_type::not_found) return 0;
    if (status.type() != fs::file_type::directory) {
        std::error_code remove_error;
        return fs::remove(path, remove_error) ? 0 : 1;
    }
    for (fs::recursive_directory_iterator it(path, walk_error), end; !walk_error && it != end;
         it.increment(walk_error)) {
        std::error_code entry_error;
        if (!it->is_directory(entry_error) || it->is_symlink(entry_error)) ++left;
    }
    return left;
}

} // namespace

std::string run_cache_program_file() { return platform::executable_file_name("program"); }

RunCacheFileSystem::~RunCacheFileSystem() = default;

std::error_code RunCacheFileSystem::rename_directory(const fs::path& from, const fs::path& to) {
    return platform::rename_directory(from, to);
}

void RunCacheFileSystem::reached(RunCachePublishPhase) {}

RunCacheStaging::~RunCacheStaging() {
    remove_tree(directory_);
    if (lease_) lease_->close();
    std::error_code ignored;
    if (!lease_path_.empty()) fs::remove(lease_path_, ignored);
}

std::optional<RunCacheStore> RunCacheStore::open(
    const fs::path& root, std::string version, std::string target, std::string* refusal,
    RunCacheFileSystem* file_system) {
    const auto fail = [&](std::string reason) -> std::optional<RunCacheStore> {
        if (refusal) *refusal = std::move(reason);
        return std::nullopt;
    };
    if (version.empty() || target.empty() || fs::path(version).filename() != version ||
        fs::path(target).filename() != target || version == "." || version == ".." ||
        target == "." || target == "..") {
        return fail("the compiler version or target cannot name a cache directory");
    }
    const auto opened = platform::open_private_root(root);
    if (!opened.path) return fail(opened.refusal);
    for (const auto* directory : {"staging", "trash"}) {
        const auto sub = platform::open_private_subdirectory(*opened.path, directory);
        if (!sub.path) return fail(sub.refusal);
    }
    RunCacheStore store;
    store.layout_ = RunCacheLayout{*opened.path, std::move(version), std::move(target)};
    store.file_system_ = file_system ? file_system : &default_file_system();
    return store;
}

std::unique_ptr<RunCacheStaging> RunCacheStore::create_staging() const {
    const auto staging = platform::open_private_subdirectory(layout_.root, "staging");
    if (!staging.path) return nullptr;
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::unique_ptr<RunCacheStaging> result(new RunCacheStaging());
        result->id_ = random_id();
        result->lease_path_ = *staging.path / (result->id_ + ".lease");
        try {
            result->lease_ = std::make_unique<platform::LockedFile>(
                result->lease_path_, platform::LockOpenMode::create_new, false);
        } catch (const platform::LockFileExistsError&) {
            result->lease_path_.clear();
            continue;
        } catch (const std::exception&) {
            result->lease_path_.clear();
            return nullptr;
        }
        const auto directory = platform::open_private_subdirectory(
            layout_.root, fs::path("staging") / result->id_);
        if (!directory.path) return nullptr;
        result->directory_ = *directory.path;
        return result;
    }
    return nullptr;
}

RunCachePublishOutcome RunCacheStore::publish(RunCacheStaging& staging, std::string_view key) const {
    if (!run_cache_digest_text(key) || staging.directory_.empty()) {
        return RunCachePublishOutcome::failed;
    }
    file_system_->reached(RunCachePublishPhase::staged);
    if (!platform::sync_directory_files(staging.directory_)) return RunCachePublishOutcome::failed;
    file_system_->reached(RunCachePublishPhase::synced);

    const auto target = layout_.entry(key);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto entries = platform::open_private_subdirectory(layout_.root, layout_.entries_relative());
        if (!entries.path) return RunCachePublishOutcome::failed;
        std::error_code error = file_system_->rename_directory(staging.directory_, target);
#ifdef _WIN32
        // A scanner may hold the new executable for a moment.
        auto delay = std::chrono::milliseconds(10);
        auto waited = std::chrono::milliseconds(0);
        while (error && classify(error) == RenameFailure::busy &&
               waited < std::chrono::milliseconds(1000)) {
            std::this_thread::sleep_for(delay);
            waited += delay;
            delay *= 2;
            error = file_system_->rename_directory(staging.directory_, target);
        }
#endif
        if (!error) {
            staging.directory_.clear();
            (void)platform::sync_directory_files(*entries.path);
            file_system_->reached(RunCachePublishPhase::renamed);
            return RunCachePublishOutcome::published;
        }
        switch (classify(error)) {
            case RenameFailure::exists:
                // Another process published this key first; its entry is
                // the same.
                return RunCachePublishOutcome::already_present;
            case RenameFailure::parent_missing:
                continue;
            default:
                return RunCachePublishOutcome::failed;
        }
    }
    return RunCachePublishOutcome::failed;
}

std::optional<fs::path> RunCacheStore::entry_directory(std::string_view key) const {
    if (!run_cache_digest_text(key)) return std::nullopt;
    const auto entry = platform::check_private_subdirectory(
        layout_.root, layout_.entries_relative() / std::string(key));
    return entry.path;
}

std::vector<std::string> RunCacheStore::index_keys(std::string_view pre_key) const {
    if (!run_cache_digest_text(pre_key)) return {};
    const auto directory = platform::check_private_subdirectory(
        layout_.root, layout_.entries_relative() / "index");
    if (!directory.path) return {};
    const auto text = platform::read_private_file(
        *directory.path / (std::string(pre_key) + ".json"), index_limit_bytes);
    if (!text) return {};
    const auto index = read_run_cache_index(*text);
    if (!index || index->pre_key != pre_key) return {};
    return index->keys;
}

bool RunCacheStore::add_to_index(std::string_view pre_key, std::string_view key) const {
    if (!run_cache_digest_text(pre_key) || !run_cache_digest_text(key)) return false;
    const auto directory = platform::open_private_subdirectory(
        layout_.root, layout_.entries_relative() / "index");
    if (!directory.path) return false;
    RunCacheIndex index{std::string(pre_key), index_keys(pre_key)};
    index = run_cache_index_with(std::move(index), std::string(key));
    return platform::replace_private_file(layout_.index(pre_key), write_run_cache_index(index));
}

bool RunCacheStore::discard_directory(const fs::path& directory) const {
    const auto trash = platform::open_private_subdirectory(layout_.root, "trash");
    if (!trash.path) return false;
    const auto destination = *trash.path / random_id();
    if (file_system_->rename_directory(directory, destination)) return false;
    (void)delete_tree(destination);
    return true;
}

void RunCacheStore::discard(std::string_view key) const {
    if (!run_cache_digest_text(key)) return;
    (void)discard_directory(layout_.entry(key));
}

void RunCacheStore::remove_dead_staging() const {
    const auto staging = layout_.staging();
    std::error_code error;
    std::vector<fs::path> leases;
    std::vector<fs::path> directories;
    for (fs::directory_iterator it(staging, error), end; !error && it != end; it.increment(error)) {
        const auto& path = it->path();
        if (path.extension() == ".lease") leases.push_back(path);
        else directories.push_back(path);
    }
    for (const auto& lease_path : leases) {
        try {
            platform::LockedFile lease(lease_path, platform::LockOpenMode::open_existing, true);
            if (!lease.present() || !lease.locked()) continue;
            auto directory = lease_path;
            directory.replace_extension();
            remove_tree(directory);
            std::error_code ignored;
            fs::remove(lease_path, ignored);
            lease.close();
        } catch (const std::exception&) {
            // A lease that cannot be inspected is left for a later scan.
        }
    }
    for (const auto& directory : directories) {
        auto lease_path = directory;
        lease_path += ".lease";
        std::error_code exists_error;
        // A staging directory is made after its lease, so one without a
        // lease belongs to no live build.
        if (!fs::exists(lease_path, exists_error) && !exists_error) remove_tree(directory);
    }
}

std::uint64_t RunCacheStore::empty_trash() const {
    std::uint64_t left = 0;
    std::error_code error;
    for (fs::directory_iterator it(layout_.trash(), error), end; !error && it != end;
         it.increment(error)) {
        left += delete_tree(it->path());
    }
    return left;
}

} // namespace quidra::toolchain
