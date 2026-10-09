#pragma once

// Bounding the run cache's disk use, and emptying it.
//
// Every entry's last use is the modification time of its `used` stamp,
// which a hit touches and a publish creates (access times are unreliable
// on noatime mounts). Sizes are allocated bytes (st_blocks * 512; the
// compressed size on Windows), summed over every compiler version and
// target below run/.
//
// A scan runs only on the miss path, after a publish, when one of these
// holds: the run/last-scan stamp is missing or older than an hour; a random
// one-in-16 draw succeeds (which bounds the growth during many builds with
// no shared counter); or the new entry is larger than a tenth of the limit.
// A scan takes run/evict.lock without waiting (another process scanning
// means this one need not), sums the entries, and when the total exceeds
// the limit moves the least recently used entries to trash/ until it is at
// most 80 % of the limit, never an entry used in the last minute. It then
// empties the trash, drops index files none of whose keys exist any more,
// removes dead staging directories and empty version and target
// directories, and stamps run/last-scan. A reader whose entry disappears
// between its checks and its use sees a miss.
//
// clean_run_cache() is `quidra cache clean`: it takes run/evict.lock
// (waiting up to 10 seconds, then going on without it), renames run/ into
// trash/ and deletes it with the rest of the trash, the compiler build id
// memos (compiler-id/) and every staging directory whose lease no process
// holds; live staging is never touched, and a run in flight sees a miss.

#include "toolchain/run_cache_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace quidra::toolchain {

inline constexpr std::uint64_t run_cache_default_limit = 1024ULL * 1024ULL * 1024ULL;

struct RunCacheEvictionPolicy {
    std::uint64_t limit_bytes{run_cache_default_limit};
    std::chrono::seconds scan_interval{3600};
    unsigned random_one_in{16};
    std::chrono::seconds recent_use{60};
    // Eviction stops at this fraction of the limit, in percent.
    unsigned low_water_percent{80};
};

// The clock and the random draw; tests inject both. Times are the file
// system's (file_time_type), compared with the stamps' modification times.
struct RunCacheEvictionEnvironment {
    std::function<std::filesystem::file_time_type()> now;
    // A value in [0, random_one_in).
    std::function<unsigned(unsigned)> draw;
};

RunCacheEvictionEnvironment run_cache_eviction_environment();

struct RunCacheUsage {
    std::uint64_t entries{};
    std::uint64_t bytes{};
};

// The entries below run/ and their allocated size.
RunCacheUsage run_cache_usage(const RunCacheStore& store);

// The allocated size of one entry directory.
std::uint64_t run_cache_entry_size(const std::filesystem::path& entry);

bool run_cache_scan_due(
    const RunCacheStore& store, std::uint64_t new_entry_bytes, const RunCacheEvictionPolicy& policy,
    const RunCacheEvictionEnvironment& environment);

struct RunCacheScan {
    // False when another process held the lock.
    bool scanned{};
    RunCacheUsage before;
    std::uint64_t evicted{};
    RunCacheUsage after;
};

RunCacheScan scan_run_cache(
    const RunCacheStore& store, const RunCacheEvictionPolicy& policy,
    const RunCacheEvictionEnvironment& environment);

// After a publish: a scan when one is due. Never throws.
void evict_run_cache_if_due(const RunCacheStore& store, std::uint64_t new_entry_bytes) noexcept;

struct RunCacheClean {
    RunCacheUsage removed;
    // Files that could not be deleted yet (in use on Windows).
    std::uint64_t files_left{};
};

RunCacheClean clean_run_cache(
    const RunCacheStore& store,
    std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(10000));

} // namespace quidra::toolchain
