#include "toolchain/run_cache_eviction.hpp"

#include "platform/cache_files.hpp"
#include "platform/file_lock.hpp"
#include "platform/private_directory.hpp"
#include "toolchain/run_cache_key.hpp"
#include "toolchain/run_cache_metadata.hpp"

#include <algorithm>
#include <memory>
#include <random>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

constexpr std::string_view lock_file = "evict.lock";
constexpr std::string_view last_scan_file = "last-scan";
constexpr std::size_t index_limit_bytes = 64 * 1024;

struct EntryUse {
    fs::path directory;
    std::uint64_t bytes{};
    fs::file_time_type used{};
};

std::vector<fs::path> subdirectories(const fs::path& directory) {
    std::vector<fs::path> result;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code status_error;
        if (it->is_directory(status_error) && !it->is_symlink(status_error)) {
            result.push_back(it->path());
        }
    }
    return result;
}

// The target directories below run/: run/<version>/<target>.
std::vector<fs::path> target_directories(const RunCacheStore& store) {
    std::vector<fs::path> result;
    for (const auto& version : subdirectories(store.layout().run())) {
        for (const auto& target : subdirectories(version)) result.push_back(target);
    }
    return result;
}

std::vector<EntryUse> entries(const RunCacheStore& store) {
    std::vector<EntryUse> result;
    for (const auto& target : target_directories(store)) {
        for (const auto& entry : subdirectories(target)) {
            if (!run_cache_digest_text(entry.filename().string())) continue;
            EntryUse use;
            use.directory = entry;
            use.bytes = run_cache_entry_size(entry);
            std::error_code error;
            const auto used = fs::last_write_time(entry / std::string(run_cache_used_file), error);
            use.used = error ? fs::file_time_type::min() : used;
            result.push_back(std::move(use));
        }
    }
    return result;
}

RunCacheUsage usage_of(const std::vector<EntryUse>& uses) {
    RunCacheUsage usage;
    for (const auto& use : uses) {
        ++usage.entries;
        usage.bytes += use.bytes;
    }
    return usage;
}

// Index files none of whose keys exist any more.
void drop_dead_indexes(const RunCacheStore& store) {
    for (const auto& target : target_directories(store)) {
        const auto index_directory = target / "index";
        std::error_code error;
        std::vector<fs::path> dead;
        for (fs::directory_iterator it(index_directory, error), end; !error && it != end;
             it.increment(error)) {
            const auto text = platform::read_private_file(it->path(), index_limit_bytes);
            const auto index = text ? read_run_cache_index(*text) : std::nullopt;
            bool alive = false;
            if (index) {
                for (const auto& key : index->keys) {
                    std::error_code exists_error;
                    if (fs::exists(target / key, exists_error)) alive = true;
                }
            }
            if (!alive) dead.push_back(it->path());
        }
        for (const auto& path : dead) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    }
}

// Version and target directories with nothing left in them.
void remove_empty_directories(const RunCacheStore& store) {
    for (const auto& version : subdirectories(store.layout().run())) {
        for (const auto& target : subdirectories(version)) {
            std::error_code ignored;
            fs::remove(target / "index", ignored);
            fs::remove(target, ignored);
        }
        std::error_code ignored;
        fs::remove(version, ignored);
    }
}

std::unique_ptr<platform::LockedFile> take_lock(const RunCacheStore& store, bool try_only) {
    const auto run = platform::open_private_subdirectory(store.layout().root, "run");
    if (!run.path) return nullptr;
    try {
        auto lock = std::make_unique<platform::LockedFile>(
            *run.path / std::string(lock_file), platform::LockOpenMode::open_always, try_only);
        if (!lock->locked()) return nullptr;
        return lock;
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

RunCacheEvictionEnvironment run_cache_eviction_environment() {
    return RunCacheEvictionEnvironment{
        [] { return fs::file_time_type::clock::now(); },
        [](unsigned bound) {
            std::random_device random;
            return bound == 0 ? 0U : static_cast<unsigned>(random() % bound);
        }};
}

std::uint64_t run_cache_entry_size(const fs::path& entry) {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (fs::directory_iterator it(entry, error), end; !error && it != end; it.increment(error)) {
        bytes += platform::allocated_size(it->path()).value_or(0);
    }
    return bytes;
}

RunCacheUsage run_cache_usage(const RunCacheStore& store) { return usage_of(entries(store)); }

bool run_cache_scan_due(
    const RunCacheStore& store, std::uint64_t new_entry_bytes, const RunCacheEvictionPolicy& policy,
    const RunCacheEvictionEnvironment& environment) {
    if (new_entry_bytes > policy.limit_bytes / 10) return true;
    std::error_code error;
    const auto last = fs::last_write_time(store.layout().run() / std::string(last_scan_file), error);
    if (error || environment.now() - last > policy.scan_interval) return true;
    return policy.random_one_in != 0 && environment.draw(policy.random_one_in) == 0;
}

RunCacheScan scan_run_cache(
    const RunCacheStore& store, const RunCacheEvictionPolicy& policy,
    const RunCacheEvictionEnvironment& environment) {
    RunCacheScan scan;
    const auto lock = take_lock(store, true);
    if (!lock) return scan;
    scan.scanned = true;
    auto uses = entries(store);
    scan.before = usage_of(uses);
    auto total = scan.before.bytes;
    if (total > policy.limit_bytes) {
        const auto low_water = policy.limit_bytes / 100 * policy.low_water_percent +
                               policy.limit_bytes % 100 * policy.low_water_percent / 100;
        std::sort(uses.begin(), uses.end(),
                  [](const EntryUse& left, const EntryUse& right) { return left.used < right.used; });
        const auto now = environment.now();
        for (const auto& use : uses) {
            if (total <= low_water) break;
            if (now - use.used < policy.recent_use) continue;
            if (!store.discard_directory(use.directory)) continue;
            total -= std::min(total, use.bytes);
            ++scan.evicted;
        }
    }
    (void)store.empty_trash();
    drop_dead_indexes(store);
    store.remove_dead_staging();
    remove_empty_directories(store);
    (void)platform::replace_private_file(store.layout().run() / std::string(last_scan_file), "");
    scan.after = run_cache_usage(store);
    return scan;
}

void evict_run_cache_if_due(const RunCacheStore& store, std::uint64_t new_entry_bytes) noexcept {
    try {
        const RunCacheEvictionPolicy policy;
        const auto environment = run_cache_eviction_environment();
        if (run_cache_scan_due(store, new_entry_bytes, policy, environment)) {
            (void)scan_run_cache(store, policy, environment);
        }
    } catch (...) {
        // Eviction only bounds disk use; a run never fails because of it.
    }
}

RunCacheClean clean_run_cache(const RunCacheStore& store, std::chrono::milliseconds lock_timeout) {
    RunCacheClean clean;
    std::unique_ptr<platform::LockedFile> lock;
    const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
    for (;;) {
        lock = take_lock(store, true);
        if (lock || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    clean.removed = run_cache_usage(store);
    std::error_code error;
    if (fs::exists(store.layout().run(), error)) {
        (void)store.discard_directory(store.layout().run());
    }
    if (fs::exists(store.layout().root / "compiler-id", error)) {
        (void)store.discard_directory(store.layout().root / "compiler-id");
    }
    store.remove_dead_staging();
    clean.files_left = store.empty_trash();
    return clean;
}

} // namespace quidra::toolchain
