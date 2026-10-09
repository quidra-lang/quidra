#pragma once

// Direct runs through the run cache: `quidra FILE.qui` and `quidra run`.
//
// A run computes the pre-key (toolchain/run_cache_key.hpp) and looks up the
// entries its index lists. An entry is a hit when its metadata is intact,
// every dependency record still holds (files read again by content, absent
// paths still absent, system and toolchain files by identity, local imports
// and installed packages resolved again, locked package trees hashed
// again), its key recomputes to its name and its program has the recorded
// size and SHA-256. Nothing is parsed or compiled. The program then runs
// exactly where an uncached run puts it, as a hidden .quidra-run-* file
// beside the source under the run registry's lease (run_artifact.hpp),
// which is a hard link to the cached file, or a copy where it cannot be
// one; the toolchain output the build printed (build.log) is written to
// stderr first, so a hit prints what the miss printed.
//
// A miss builds as an uncached run does, in a staging directory of the
// cache, with the compile inputs recorded and the native link in run mode
// (toolchain/native_link_recipe.hpp), and runs the result the same way. The
// entry is published only when everything the build read is described:
// the compile inputs are complete, the toolchain snapshot and its
// dependency records are complete, no package native source or user header
// uses __DATE__, __TIME__ or __TIMESTAMP__, no native input changed during
// the build, and no dependency changed in the 2 seconds before the build
// started (a file changed within the file system's time resolution could
// otherwise keep an identity it no longer deserves).
//
// The cache never changes what a run does or prints. Whenever it cannot
// serve a run correctly (no cache directory, a directory another user
// owns, a toolchain that cannot be described, a staging directory that
// cannot be made, a program that cannot be placed beside the source) the
// run takes the uncached path the caller passes, silently. A cached program
// that cannot be started is discarded and built once more. A failed build
// stores nothing and prints what an uncached build prints.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace quidra::run_cache {

struct DirectRun {
    std::filesystem::path source;
    std::size_t max_errors{20};
    std::vector<std::string> program_arguments;
    // As the command was given them.
    std::vector<std::filesystem::path> link_inputs;
};

// Runs `run` through the cache and returns its exit status. `uncached`
// runs it as `--no-cache` does.
int run_cached(const DirectRun& run, const std::function<int()>& uncached);

// `quidra cache clean`: empties the cache (toolchain/run_cache_eviction.hpp)
// and prints "removed N entries (X MiB)" on `out`, and how many files in use
// were left where some were; status 0. The one place where the cache explains
// itself: a cache directory that cannot be used (another user's, a symbolic
// link below it, not a directory) is left alone, with the reason on `err`
// and status 1.
int clean_cache(std::ostream& out, std::ostream& err);

} // namespace quidra::run_cache
