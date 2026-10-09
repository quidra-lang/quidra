#pragma once

// The static archiver of a library build (`quidra build --lib`): merges
// objects and the members of other static archives into one archive.
//
// Owns the choice of the tool and its command line:
// - Apple platforms: `libtool -static -o OUT MEMBERS...`; QUIDRA_LIBTOOL,
//   when set and not empty, names the program;
// - Windows: `lib.exe /NOLOGO /OUT:OUT MEMBERS...` (llvm-lib takes the same
//   arguments); QUIDRA_AR, when set and not empty, names the program;
// - elsewhere: `ar -M` with an MRI script (CREATE, ADDMOD per object,
//   ADDLIB per archive, SAVE), which GNU ar and llvm-ar read; QUIDRA_AR,
//   when set and not empty, names the program.
// The archive is written to a temporary name beside OUT and renamed over it
// once the tool succeeded, so a failed build leaves no partial archive.

#include <filesystem>
#include <vector>

namespace quidra::toolchain {

// Writes `output` from `objects` (added as members) and `archives` (whose
// members are added). Throws std::runtime_error naming the tool when it
// cannot be run or fails.
void create_static_archive(const std::filesystem::path& output,
                           const std::vector<std::filesystem::path>& objects,
                           const std::vector<std::filesystem::path>& archives);

} // namespace quidra::toolchain
